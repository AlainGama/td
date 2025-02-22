//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/files/FileLoadManager.h"

#include "td/telegram/Global.h"
#include "td/telegram/net/DcId.h"
#include "td/telegram/TdParameters.h"

#include "td/utils/common.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/port/path.h"
#include "td/utils/SliceBuilder.h"

namespace td {

FileLoadManager::FileLoadManager(ActorShared<Callback> callback, ActorShared<> parent)
    : callback_(std::move(callback)), parent_(std::move(parent)) {
}

void FileLoadManager::start_up() {
  constexpr int64 MAX_UPLOAD_RESOURCE_LIMIT = 4 << 20;
  upload_resource_manager_ = create_actor<ResourceManager>("UploadResourceManager", MAX_UPLOAD_RESOURCE_LIMIT,
                                                           !G()->parameters().use_file_db /*tdlib_engine*/
                                                               ? ResourceManager::Mode::Greedy
                                                               : ResourceManager::Mode::Baseline);
  if (G()->get_option_boolean("is_premium")) {
    max_download_resource_limit_ *= 8;
  }
}

ActorOwn<ResourceManager> &FileLoadManager::get_download_resource_manager(bool is_small, DcId dc_id) {
  auto &actor = is_small ? download_small_resource_manager_map_[dc_id] : download_resource_manager_map_[dc_id];
  if (actor.empty()) {
    actor = create_actor<ResourceManager>(
        PSLICE() << "DownloadResourceManager " << tag("is_small", is_small) << tag("dc_id", dc_id),
        max_download_resource_limit_, ResourceManager::Mode::Baseline);
  }
  return actor;
}

void FileLoadManager::download(QueryId query_id, const FullRemoteFileLocation &remote_location,
                               const LocalFileLocation &local, int64 size, string name,
                               const FileEncryptionKey &encryption_key, bool search_file, int64 offset, int64 limit,
                               int8 priority) {
  if (stop_flag_) {
    return;
  }
  NodeId node_id = nodes_container_.create(Node());
  Node *node = nodes_container_.get(node_id);
  CHECK(node);
  node->query_id_ = query_id;
  auto callback = make_unique<FileDownloaderCallback>(actor_shared(this, node_id));
  bool is_small = size < 20 * 1024;
  node->loader_ =
      create_actor<FileDownloader>("Downloader", remote_location, local, size, std::move(name), encryption_key,
                                   is_small, search_file, offset, limit, std::move(callback));
  DcId dc_id = remote_location.is_web() ? G()->get_webfile_dc_id() : remote_location.get_dc_id();
  auto &resource_manager = get_download_resource_manager(is_small, dc_id);
  send_closure(resource_manager, &ResourceManager::register_worker,
               ActorShared<FileLoaderActor>(node->loader_.get(), static_cast<uint64>(-1)), priority);
  bool is_inserted = query_id_to_node_id_.emplace(query_id, node_id).second;
  CHECK(is_inserted);
}

void FileLoadManager::upload(QueryId query_id, const LocalFileLocation &local_location,
                             const RemoteFileLocation &remote_location, int64 expected_size,
                             const FileEncryptionKey &encryption_key, int8 priority, vector<int> bad_parts) {
  if (stop_flag_) {
    return;
  }
  NodeId node_id = nodes_container_.create(Node());
  Node *node = nodes_container_.get(node_id);
  CHECK(node);
  node->query_id_ = query_id;
  auto callback = make_unique<FileUploaderCallback>(actor_shared(this, node_id));
  node->loader_ = create_actor<FileUploader>("Uploader", local_location, remote_location, expected_size, encryption_key,
                                             std::move(bad_parts), std::move(callback));
  send_closure(upload_resource_manager_, &ResourceManager::register_worker,
               ActorShared<FileLoaderActor>(node->loader_.get(), static_cast<uint64>(-1)), priority);
  bool is_inserted = query_id_to_node_id_.emplace(query_id, node_id).second;
  CHECK(is_inserted);
}

void FileLoadManager::upload_by_hash(QueryId query_id, const FullLocalFileLocation &local_location, int64 size,
                                     int8 priority) {
  if (stop_flag_) {
    return;
  }
  NodeId node_id = nodes_container_.create(Node());
  Node *node = nodes_container_.get(node_id);
  CHECK(node);
  node->query_id_ = query_id;
  auto callback = make_unique<FileHashUploaderCallback>(actor_shared(this, node_id));
  node->loader_ = create_actor<FileHashUploader>("HashUploader", local_location, size, std::move(callback));
  send_closure(upload_resource_manager_, &ResourceManager::register_worker,
               ActorShared<FileLoaderActor>(node->loader_.get(), static_cast<uint64>(-1)), priority);
  bool is_inserted = query_id_to_node_id_.emplace(query_id, node_id).second;
  CHECK(is_inserted);
}

void FileLoadManager::update_priority(QueryId query_id, int8 priority) {
  if (stop_flag_) {
    return;
  }
  auto it = query_id_to_node_id_.find(query_id);
  if (it == query_id_to_node_id_.end()) {
    return;
  }
  auto node = nodes_container_.get(it->second);
  if (node == nullptr) {
    return;
  }
  send_closure(node->loader_, &FileLoaderActor::update_priority, priority);
}

void FileLoadManager::from_bytes(QueryId query_id, FileType type, BufferSlice bytes, string name) {
  if (stop_flag_) {
    return;
  }
  NodeId node_id = nodes_container_.create(Node());
  Node *node = nodes_container_.get(node_id);
  CHECK(node);
  node->query_id_ = query_id;
  auto callback = make_unique<FileFromBytesCallback>(actor_shared(this, node_id));
  node->loader_ =
      create_actor<FileFromBytes>("FromBytes", type, std::move(bytes), std::move(name), std::move(callback));
  bool is_inserted = query_id_to_node_id_.emplace(query_id, node_id).second;
  CHECK(is_inserted);
}

void FileLoadManager::get_content(string file_path, Promise<BufferSlice> promise) {
  promise.set_result(read_file(file_path));
}

void FileLoadManager::read_file_part(string file_path, int64 offset, int64 count, Promise<string> promise) {
  promise.set_result(read_file_str(file_path, count, offset));
}

void FileLoadManager::unlink_file(string file_path, Promise<Unit> promise) {
  unlink(file_path).ignore();
  promise.set_value(Unit());
}

void FileLoadManager::check_full_local_location(FullLocalLocationInfo local_info, bool skip_file_size_checks,
                                                Promise<FullLocalLocationInfo> promise) {
  promise.set_result(::td::check_full_local_location(std::move(local_info), skip_file_size_checks));
}

void FileLoadManager::check_partial_local_location(PartialLocalFileLocation partial, Promise<Unit> promise) {
  auto status = ::td::check_partial_local_location(partial);
  if (status.is_error()) {
    promise.set_error(std::move(status));
  } else {
    promise.set_value(Unit());
  }
}

void FileLoadManager::cancel(QueryId query_id) {
  if (stop_flag_) {
    return;
  }
  auto it = query_id_to_node_id_.find(query_id);
  if (it == query_id_to_node_id_.end()) {
    return;
  }
  on_error_impl(it->second, Status::Error(-1, "Canceled"));
}

void FileLoadManager::update_local_file_location(QueryId query_id, const LocalFileLocation &local) {
  if (stop_flag_) {
    return;
  }
  auto it = query_id_to_node_id_.find(query_id);
  if (it == query_id_to_node_id_.end()) {
    return;
  }
  auto node = nodes_container_.get(it->second);
  if (node == nullptr) {
    return;
  }
  send_closure(node->loader_, &FileLoaderActor::update_local_file_location, local);
}

void FileLoadManager::update_downloaded_part(QueryId query_id, int64 offset, int64 limit) {
  if (stop_flag_) {
    return;
  }
  auto it = query_id_to_node_id_.find(query_id);
  if (it == query_id_to_node_id_.end()) {
    return;
  }
  auto node = nodes_container_.get(it->second);
  if (node == nullptr) {
    return;
  }
  send_closure(node->loader_, &FileLoaderActor::update_downloaded_part, offset, limit, max_download_resource_limit_);
}

void FileLoadManager::hangup() {
  nodes_container_.for_each([](auto query_id, auto &node) { node.loader_.reset(); });
  stop_flag_ = true;
  loop();
}

void FileLoadManager::on_start_download() {
  auto node_id = get_link_token();
  auto node = nodes_container_.get(node_id);
  if (node == nullptr) {
    return;
  }
  if (!stop_flag_) {
    send_closure(callback_, &Callback::on_start_download, node->query_id_);
  }
}

void FileLoadManager::on_partial_download(PartialLocalFileLocation partial_local, int64 ready_size, int64 size) {
  auto node_id = get_link_token();
  auto node = nodes_container_.get(node_id);
  if (node == nullptr) {
    return;
  }
  if (!stop_flag_) {
    send_closure(callback_, &Callback::on_partial_download, node->query_id_, std::move(partial_local), ready_size,
                 size);
  }
}

void FileLoadManager::on_hash(string hash) {
  auto node_id = get_link_token();
  auto node = nodes_container_.get(node_id);
  if (node == nullptr) {
    return;
  }
  if (!stop_flag_) {
    send_closure(callback_, &Callback::on_hash, node->query_id_, std::move(hash));
  }
}

void FileLoadManager::on_partial_upload(PartialRemoteFileLocation partial_remote, int64 ready_size) {
  auto node_id = get_link_token();
  auto node = nodes_container_.get(node_id);
  if (node == nullptr) {
    return;
  }
  if (!stop_flag_) {
    send_closure(callback_, &Callback::on_partial_upload, node->query_id_, std::move(partial_remote), ready_size);
  }
}

void FileLoadManager::on_ok_download(FullLocalFileLocation local, int64 size, bool is_new) {
  auto node_id = get_link_token();
  auto node = nodes_container_.get(node_id);
  if (node == nullptr) {
    return;
  }
  if (!stop_flag_) {
    send_closure(callback_, &Callback::on_download_ok, node->query_id_, std::move(local), size, is_new);
  }
  close_node(node_id);
  loop();
}

void FileLoadManager::on_ok_upload(FileType file_type, PartialRemoteFileLocation remote, int64 size) {
  auto node_id = get_link_token();
  auto node = nodes_container_.get(node_id);
  if (node == nullptr) {
    return;
  }
  if (!stop_flag_) {
    send_closure(callback_, &Callback::on_upload_ok, node->query_id_, file_type, std::move(remote), size);
  }
  close_node(node_id);
  loop();
}

void FileLoadManager::on_ok_upload_full(FullRemoteFileLocation remote) {
  auto node_id = get_link_token();
  auto node = nodes_container_.get(node_id);
  if (node == nullptr) {
    return;
  }
  if (!stop_flag_) {
    send_closure(callback_, &Callback::on_upload_full_ok, node->query_id_, std::move(remote));
  }
  close_node(node_id);
  loop();
}

void FileLoadManager::on_error(Status status) {
  auto node_id = get_link_token();
  on_error_impl(node_id, std::move(status));
}

void FileLoadManager::on_error_impl(NodeId node_id, Status status) {
  auto node = nodes_container_.get(node_id);
  if (node == nullptr) {
    status.ignore();
    return;
  }
  if (!stop_flag_) {
    send_closure(callback_, &Callback::on_error, node->query_id_, std::move(status));
  }
  close_node(node_id);
  loop();
}

void FileLoadManager::hangup_shared() {
  auto node_id = get_link_token();
  on_error_impl(node_id, Status::Error(-1, "Canceled"));
}

void FileLoadManager::loop() {
  if (stop_flag_ && nodes_container_.empty()) {
    stop();
  }
}

void FileLoadManager::close_node(NodeId node_id) {
  auto node = nodes_container_.get(node_id);
  CHECK(node);
  query_id_to_node_id_.erase(node->query_id_);
  nodes_container_.erase(node_id);
}

}  // namespace td
