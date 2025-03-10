/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License. */

#include "paddle/fluid/framework/data_set.h"

#include <algorithm>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "paddle/fluid/framework/data_feed_factory.h"
#include "paddle/fluid/framework/fleet/box_wrapper.h"
#include "paddle/fluid/framework/fleet/fleet_wrapper.h"
#include "paddle/fluid/framework/io/fs.h"
#include "paddle/fluid/platform/monitor.h"
#include "paddle/fluid/platform/timer.h"
#include "xxhash.h"  // NOLINT

#if defined _WIN32 || defined __APPLE__
#else
#define _LINUX
#endif

DECLARE_bool(padbox_dataset_disable_shuffle);
DECLARE_bool(padbox_dataset_disable_polling);
DECLARE_bool(padbox_dataset_enable_unrollinstance);

namespace paddle {
namespace framework {

// constructor
template <typename T>
DatasetImpl<T>::DatasetImpl() {
  VLOG(3) << "DatasetImpl<T>::DatasetImpl() constructor";
  thread_num_ = 1;
  trainer_num_ = 1;
  channel_num_ = 1;
  file_idx_ = 0;
  total_fea_num_ = 0;
  cur_channel_ = 0;
  fleet_send_batch_size_ = 1024;
  fleet_send_sleep_seconds_ = 0;
  merge_by_insid_ = false;
  merge_by_sid_ = true;
  enable_pv_merge_ = false;
  merge_size_ = 2;
  parse_ins_id_ = false;
  parse_content_ = false;
  parse_logkey_ = false;
  preload_thread_num_ = 0;
  global_index_ = 0;
}

// set filelist, file_idx_ will reset to zero.
template <typename T>
void DatasetImpl<T>::SetFileList(const std::vector<std::string>& filelist) {
  VLOG(3) << "filelist size: " << filelist.size();
  filelist_ = filelist;
  file_idx_ = 0;
}

// set expect thread num. actually it may change
template <typename T>
void DatasetImpl<T>::SetThreadNum(int thread_num) {
  VLOG(3) << "SetThreadNum thread_num=" << thread_num;
  thread_num_ = thread_num;
}

// if you run distributed, and want to do global shuffle,
// set this before global shuffle.
// be sure you call CreateReaders before SetTrainerNum
template <typename T>
void DatasetImpl<T>::SetTrainerNum(int trainer_num) {
  trainer_num_ = trainer_num;
}

// if you run distributed, and want to do global shuffle,
// set this before global shuffle.
// be sure you call CreateReaders before SetFleetSendBatchSize
template <typename T>
void DatasetImpl<T>::SetFleetSendBatchSize(int64_t size) {
  fleet_send_batch_size_ = size;
}

template <typename T>
void DatasetImpl<T>::SetHdfsConfig(const std::string& fs_name,
                                   const std::string& fs_ugi) {
  fs_name_ = fs_name;
  fs_ugi_ = fs_ugi;
  std::string cmd = std::string("$HADOOP_HOME/bin/hadoop fs");
  cmd += " -D fs.default.name=" + fs_name;
  cmd += " -D hadoop.job.ugi=" + fs_ugi;
  cmd += " -Ddfs.client.block.write.retries=15 -Ddfs.rpc.timeout=500000";
  paddle::framework::hdfs_set_command(cmd);
}

template <typename T>
void DatasetImpl<T>::SetDownloadCmd(const std::string& download_cmd) {
  paddle::framework::set_download_command(download_cmd);
}

template <typename T>
std::string DatasetImpl<T>::GetDownloadCmd() {
  return paddle::framework::download_cmd();
}

template <typename T>
void DatasetImpl<T>::SetDataFeedDesc(const std::string& data_feed_desc_str) {
  google::protobuf::TextFormat::ParseFromString(data_feed_desc_str,
                                                &data_feed_desc_);
}

template <typename T>
void DatasetImpl<T>::SetChannelNum(int channel_num) {
  channel_num_ = channel_num;
}

template <typename T>
void DatasetImpl<T>::SetParseInsId(bool parse_ins_id) {
  parse_ins_id_ = parse_ins_id;
}

template <typename T>
void DatasetImpl<T>::SetParseContent(bool parse_content) {
  parse_content_ = parse_content;
}

template <typename T>
void DatasetImpl<T>::SetParseLogKey(bool parse_logkey) {
  parse_logkey_ = parse_logkey;
}

template <typename T>
void DatasetImpl<T>::SetMergeByInsId(int merge_size) {
  merge_by_insid_ = true;
  parse_ins_id_ = true;
  merge_size_ = merge_size;
}

template <typename T>
void DatasetImpl<T>::SetMergeBySid(bool is_merge) {
  merge_by_sid_ = is_merge;
}

template <typename T>
void DatasetImpl<T>::SetEnablePvMerge(bool enable_pv_merge) {
  enable_pv_merge_ = enable_pv_merge;
}

template <typename T>
void DatasetImpl<T>::SetGenerateUniqueFeasign(bool gen_uni_feasigns) {
  gen_uni_feasigns_ = gen_uni_feasigns;
  VLOG(3) << "Set generate unique feasigns: " << gen_uni_feasigns;
}

template <typename T>
void DatasetImpl<T>::SetFeaEval(bool fea_eval, int record_candidate_size) {
  slots_shuffle_fea_eval_ = fea_eval;
  slots_shuffle_rclist_.ReSize(record_candidate_size);
  VLOG(3) << "SetFeaEval fea eval mode: " << fea_eval
          << " with record candidate size: " << record_candidate_size;
}

template <typename T>
std::vector<paddle::framework::DataFeed*> DatasetImpl<T>::GetReaders() {
  std::vector<paddle::framework::DataFeed*> ret;
  ret.reserve(readers_.size());
  for (auto i : readers_) {
    ret.push_back(i.get());
  }
  return ret;
}

template <typename T>
void DatasetImpl<T>::CreateChannel() {
  if (input_channel_ == nullptr) {
    input_channel_ = paddle::framework::MakeChannel<T>();
  }
  if (multi_output_channel_.size() == 0) {
    multi_output_channel_.reserve(channel_num_);
    for (int i = 0; i < channel_num_; ++i) {
      multi_output_channel_.push_back(paddle::framework::MakeChannel<T>());
    }
  }
  if (multi_consume_channel_.size() == 0) {
    multi_consume_channel_.reserve(channel_num_);
    for (int i = 0; i < channel_num_; ++i) {
      multi_consume_channel_.push_back(paddle::framework::MakeChannel<T>());
    }
  }
  if (input_pv_channel_ == nullptr) {
    input_pv_channel_ = paddle::framework::MakeChannel<PvInstance>();
  }
  if (multi_pv_output_.size() == 0) {
    multi_pv_output_.reserve(channel_num_);
    for (int i = 0; i < channel_num_; ++i) {
      multi_pv_output_.push_back(paddle::framework::MakeChannel<PvInstance>());
    }
  }
  if (multi_pv_consume_.size() == 0) {
    multi_pv_consume_.reserve(channel_num_);
    for (int i = 0; i < channel_num_; ++i) {
      multi_pv_consume_.push_back(paddle::framework::MakeChannel<PvInstance>());
    }
  }
  if (input_ptr_channel_ == nullptr) {
    input_ptr_channel_ = paddle::framework::MakeChannel<T*>();
  }
  if (output_ptr_channel_.size() == 0) {
    output_ptr_channel_.reserve(channel_num_);
    for (int i = 0; i < channel_num_; ++i) {
      output_ptr_channel_.push_back(paddle::framework::MakeChannel<T*>());
    }
  }
  if (consume_ptr_channel_.size() == 0) {
    consume_ptr_channel_.reserve(channel_num_);
    for (int i = 0; i < channel_num_; ++i) {
      consume_ptr_channel_.push_back(paddle::framework::MakeChannel<T*>());
    }
  }
}

// if sent message between workers, should first call this function
template <typename T>
void DatasetImpl<T>::RegisterClientToClientMsgHandler() {
  auto fleet_ptr = FleetWrapper::GetInstance();
  VLOG(3) << "RegisterClientToClientMsgHandler";
  fleet_ptr->RegisterClientToClientMsgHandler(
      0, [this](int msg_type, int client_id, const std::string& msg) -> int {
        return this->ReceiveFromClient(msg_type, client_id, msg);
      });
  VLOG(3) << "RegisterClientToClientMsgHandler done";
}

// load data into memory, Dataset hold this memory,
// which will later be fed into readers' channel
template <typename T>
void DatasetImpl<T>::LoadIntoMemory() {
  VLOG(3) << "DatasetImpl<T>::LoadIntoMemory() begin";
  platform::Timer timeline;
  timeline.Start();
  std::vector<std::thread> load_threads;
  for (int64_t i = 0; i < thread_num_; ++i) {
    load_threads.push_back(std::thread(
        &paddle::framework::DataFeed::LoadIntoMemory, readers_[i].get()));
  }
  for (std::thread& t : load_threads) {
    t.join();
  }
  input_channel_->Close();
  int64_t in_chan_size = input_channel_->Size();
  input_channel_->SetBlockSize(in_chan_size / thread_num_ + 1);

  timeline.Pause();
  VLOG(3) << "DatasetImpl<T>::LoadIntoMemory() end"
          << ", memory data size=" << input_channel_->Size()
          << ", cost time=" << timeline.ElapsedSec() << " seconds";
}

template <typename T>
void DatasetImpl<T>::PreLoadIntoMemory() {
  VLOG(3) << "DatasetImpl<T>::PreLoadIntoMemory() begin";
  if (preload_thread_num_ != 0) {
    CHECK(static_cast<size_t>(preload_thread_num_) == preload_readers_.size());
    preload_threads_.clear();
    for (int64_t i = 0; i < preload_thread_num_; ++i) {
      preload_threads_.push_back(
          std::thread(&paddle::framework::DataFeed::LoadIntoMemory,
                      preload_readers_[i].get()));
    }
  } else {
    CHECK(static_cast<size_t>(thread_num_) == readers_.size());
    preload_threads_.clear();
    for (int64_t i = 0; i < thread_num_; ++i) {
      preload_threads_.push_back(std::thread(
          &paddle::framework::DataFeed::LoadIntoMemory, readers_[i].get()));
    }
  }
  VLOG(3) << "DatasetImpl<T>::PreLoadIntoMemory() end";
}

template <typename T>
void DatasetImpl<T>::WaitPreLoadDone() {
  VLOG(3) << "DatasetImpl<T>::WaitPreLoadDone() begin";
  for (std::thread& t : preload_threads_) {
    t.join();
  }
  input_channel_->Close();
  int64_t in_chan_size = input_channel_->Size();
  input_channel_->SetBlockSize(in_chan_size / thread_num_ + 1);
  VLOG(3) << "DatasetImpl<T>::WaitPreLoadDone() end";
}

// release memory data
template <typename T>
void DatasetImpl<T>::ReleaseMemory() {
  release_thread_ = new std::thread(&DatasetImpl<T>::ReleaseMemoryFun, this);
}
template <typename T>
void DatasetImpl<T>::ReleaseMemoryFun() {
  VLOG(0) << "DatasetImpl<T>::ReleaseMemory() begin";
  if (input_channel_) {
    input_channel_->Clear();
    input_channel_ = nullptr;
  }
  for (size_t i = 0; i < multi_output_channel_.size(); ++i) {
    if (!multi_output_channel_[i]) {
      continue;
    }
    multi_output_channel_[i]->Clear();
    multi_output_channel_[i] = nullptr;
  }
  std::vector<paddle::framework::Channel<T>>().swap(multi_output_channel_);
  for (size_t i = 0; i < multi_consume_channel_.size(); ++i) {
    if (!multi_consume_channel_[i]) {
      continue;
    }
    multi_consume_channel_[i]->Clear();
    multi_consume_channel_[i] = nullptr;
  }
  std::vector<paddle::framework::Channel<T>>().swap(multi_consume_channel_);
  if (input_pv_channel_) {
    input_pv_channel_->Clear();
    input_pv_channel_ = nullptr;
  }
  for (size_t i = 0; i < multi_pv_output_.size(); ++i) {
    if (!multi_pv_output_[i]) {
      continue;
    }
    multi_pv_output_[i]->Clear();
    multi_pv_output_[i] = nullptr;
  }
  std::vector<paddle::framework::Channel<PvInstance>>().swap(multi_pv_output_);
  for (size_t i = 0; i < multi_pv_consume_.size(); ++i) {
    if (!multi_pv_consume_[i]) {
      continue;
    }
    multi_pv_consume_[i]->Clear();
    multi_pv_consume_[i] = nullptr;
  }
  std::vector<paddle::framework::Channel<PvInstance>>().swap(multi_pv_consume_);

  std::vector<std::shared_ptr<paddle::framework::DataFeed>>().swap(readers_);
  input_records_.clear();
  std::vector<T>().swap(input_records_);
  std::vector<T>().swap(slots_shuffle_original_data_);
  VLOG(3) << "DatasetImpl<T>::ReleaseMemory() end";
  VLOG(3) << "total_feasign_num_(" << STAT_GET(STAT_total_feasign_num_in_mem)
          << ") - current_fea_num_(" << total_fea_num_ << ") = ("
          << STAT_GET(STAT_total_feasign_num_in_mem) - total_fea_num_
          << ")";  // For Debug
  STAT_SUB(STAT_total_feasign_num_in_mem, total_fea_num_);
}

// do local shuffle
template <typename T>
void DatasetImpl<T>::LocalShuffle() {
  VLOG(3) << "DatasetImpl<T>::LocalShuffle() begin";
  platform::Timer timeline;
  timeline.Start();

  if (!input_channel_ || input_channel_->Size() == 0) {
    VLOG(3) << "DatasetImpl<T>::LocalShuffle() end, no data to shuffle";
    return;
  }
  auto fleet_ptr = FleetWrapper::GetInstance();
  input_channel_->Close();
  std::vector<T> data;
  input_channel_->ReadAll(data);
  std::shuffle(data.begin(), data.end(), fleet_ptr->LocalRandomEngine());
  input_channel_->Open();
  input_channel_->Write(std::move(data));
  data.clear();
  data.shrink_to_fit();
  input_channel_->Close();

  timeline.Pause();
  VLOG(3) << "DatasetImpl<T>::LocalShuffle() end, cost time="
          << timeline.ElapsedSec() << " seconds";
}

template <typename T>
void DatasetImpl<T>::GlobalShuffle(int thread_num) {
#ifdef PADDLE_WITH_PSLIB
  VLOG(3) << "DatasetImpl<T>::GlobalShuffle() begin";
  platform::Timer timeline;
  timeline.Start();
  auto fleet_ptr = FleetWrapper::GetInstance();

  if (!input_channel_ || input_channel_->Size() == 0) {
    VLOG(3) << "DatasetImpl<T>::GlobalShuffle() end, no data to shuffle";
    return;
  }

  // local shuffle
  input_channel_->Close();
  std::vector<T> data;
  input_channel_->ReadAll(data);
  std::shuffle(data.begin(), data.end(), fleet_ptr->LocalRandomEngine());
  input_channel_->Open();
  input_channel_->Write(std::move(data));
  data.clear();
  data.shrink_to_fit();

  input_channel_->Close();
  input_channel_->SetBlockSize(fleet_send_batch_size_);
  VLOG(3) << "DatasetImpl<T>::GlobalShuffle() input_channel_ size "
          << input_channel_->Size();

  auto get_client_id = [this, fleet_ptr](const T& data) -> size_t {
    if (!this->merge_by_insid_) {
      return fleet_ptr->LocalRandomEngine()() % this->trainer_num_;
    } else {
      return XXH64(data.ins_id_.data(), data.ins_id_.length(), 0) %
             this->trainer_num_;
    }
  };

  auto global_shuffle_func = [this, get_client_id]() {
    auto fleet_ptr = FleetWrapper::GetInstance();
    std::vector<T> data;
    while (this->input_channel_->Read(data)) {
      std::vector<paddle::framework::BinaryArchive> ars(this->trainer_num_);
      for (auto& t : data) {
        auto client_id = get_client_id(t);
        ars[client_id] << t;
      }
      std::vector<std::future<int32_t>> total_status;
      std::vector<int> send_index(this->trainer_num_);
      for (int i = 0; i < this->trainer_num_; ++i) {
        send_index[i] = i;
      }
      std::shuffle(send_index.begin(), send_index.end(),
                   fleet_ptr->LocalRandomEngine());
      for (int index = 0; index < this->trainer_num_; ++index) {
        int i = send_index[index];
        if (ars[i].Length() == 0) {
          continue;
        }
        std::string msg(ars[i].Buffer(), ars[i].Length());
        auto ret = fleet_ptr->SendClientToClientMsg(0, i, msg);
        total_status.push_back(std::move(ret));
      }
      for (auto& t : total_status) {
        t.wait();
      }
      ars.clear();
      ars.shrink_to_fit();
      data.clear();
      data.shrink_to_fit();
      // currently we find bottleneck is server not able to handle large data
      // in time, so we can remove this sleep and set fleet_send_batch_size to
      // 1024, and set server thread to 24.
      if (fleet_send_sleep_seconds_ != 0) {
        sleep(this->fleet_send_sleep_seconds_);
      }
    }
  };

  std::vector<std::thread> global_shuffle_threads;
  if (thread_num == -1) {
    thread_num = thread_num_;
  }
  VLOG(3) << "start global shuffle threads, num = " << thread_num;
  for (int i = 0; i < thread_num; ++i) {
    global_shuffle_threads.push_back(std::thread(global_shuffle_func));
  }
  for (std::thread& t : global_shuffle_threads) {
    t.join();
  }
  global_shuffle_threads.clear();
  global_shuffle_threads.shrink_to_fit();
  input_channel_->Clear();
  timeline.Pause();
  VLOG(3) << "DatasetImpl<T>::GlobalShuffle() end, cost time="
          << timeline.ElapsedSec() << " seconds";
#endif
}

template <typename T>
void DatasetImpl<T>::DynamicAdjustChannelNum(int channel_num,
                                             bool discard_remaining_ins) {
  if (channel_num_ == channel_num) {
    VLOG(3) << "DatasetImpl<T>::DynamicAdjustChannelNum channel_num_="
            << channel_num_ << ", channel_num_=channel_num, no need to adjust";
    return;
  }
  VLOG(3) << "adjust channel num from " << channel_num_ << " to "
          << channel_num;
  channel_num_ = channel_num;
  std::vector<paddle::framework::Channel<T>>* origin_channels = nullptr;
  std::vector<paddle::framework::Channel<T>>* other_channels = nullptr;
  std::vector<paddle::framework::Channel<PvInstance>>* origin_pv_channels =
      nullptr;
  std::vector<paddle::framework::Channel<PvInstance>>* other_pv_channels =
      nullptr;

  // find out which channel (output or consume) has data
  int cur_channel = 0;
  uint64_t output_channels_data_size = 0;
  uint64_t consume_channels_data_size = 0;
  CHECK(multi_output_channel_.size() == multi_consume_channel_.size());
  for (size_t i = 0; i < multi_output_channel_.size(); ++i) {
    output_channels_data_size += multi_output_channel_[i]->Size();
    consume_channels_data_size += multi_consume_channel_[i]->Size();
  }
  if (output_channels_data_size != 0) {
    CHECK(consume_channels_data_size == 0);  // NOLINT
    cur_channel = 0;
  } else {
    CHECK(output_channels_data_size == 0);  // NOLINT
    cur_channel = 1;
  }
  if (cur_channel == 0) {
    origin_channels = &multi_output_channel_;
    other_channels = &multi_consume_channel_;
    origin_pv_channels = &multi_pv_output_;
    other_pv_channels = &multi_pv_consume_;
  } else {
    origin_channels = &multi_consume_channel_;
    other_channels = &multi_output_channel_;
    origin_pv_channels = &multi_pv_consume_;
    other_pv_channels = &multi_pv_output_;
  }
  CHECK(origin_channels != nullptr);     // NOLINT
  CHECK(other_channels != nullptr);      // NOLINT
  CHECK(origin_pv_channels != nullptr);  // NOLINT
  CHECK(other_pv_channels != nullptr);   // NOLINT

  paddle::framework::Channel<T> total_data_channel =
      paddle::framework::MakeChannel<T>();
  std::vector<paddle::framework::Channel<T>> new_channels;
  std::vector<paddle::framework::Channel<T>> new_other_channels;
  std::vector<paddle::framework::Channel<PvInstance>> new_pv_channels;
  std::vector<paddle::framework::Channel<PvInstance>> new_other_pv_channels;

  std::vector<T> local_vec;
  for (size_t i = 0; i < origin_channels->size(); ++i) {
    local_vec.clear();
    (*origin_channels)[i]->Close();
    (*origin_channels)[i]->ReadAll(local_vec);
    total_data_channel->Write(std::move(local_vec));
  }
  total_data_channel->Close();
  if (static_cast<int>(total_data_channel->Size()) >= channel_num) {
    total_data_channel->SetBlockSize(total_data_channel->Size() / channel_num +
                                     (discard_remaining_ins ? 0 : 1));
  }
  if (static_cast<int>(input_channel_->Size()) >= channel_num) {
    input_channel_->SetBlockSize(input_channel_->Size() / channel_num +
                                 (discard_remaining_ins ? 0 : 1));
  }
  if (static_cast<int>(input_ptr_channel_->Size()) >= channel_num) {
    input_ptr_channel_->SetBlockSize(input_ptr_channel_->Size() / channel_num +
                                     (discard_remaining_ins ? 0 : 1));
  }
  if (static_cast<int>(input_pv_channel_->Size()) >= channel_num) {
    input_pv_channel_->SetBlockSize(input_pv_channel_->Size() / channel_num +
                                    (discard_remaining_ins ? 0 : 1));
    VLOG(3) << "now input_pv_channle block size is "
            << input_pv_channel_->BlockSize();
  }

  for (int i = 0; i < channel_num; ++i) {
    local_vec.clear();
    total_data_channel->Read(local_vec);
    new_other_channels.push_back(paddle::framework::MakeChannel<T>());
    new_channels.push_back(paddle::framework::MakeChannel<T>());
    new_channels[i]->Write(std::move(local_vec));
    new_other_pv_channels.push_back(
        paddle::framework::MakeChannel<PvInstance>());
    new_pv_channels.push_back(paddle::framework::MakeChannel<PvInstance>());
  }

  total_data_channel->Clear();
  origin_channels->clear();
  other_channels->clear();
  *origin_channels = new_channels;
  *other_channels = new_other_channels;

  origin_pv_channels->clear();
  other_pv_channels->clear();
  *origin_pv_channels = new_pv_channels;
  *other_pv_channels = new_other_pv_channels;

  new_channels.clear();
  new_other_channels.clear();
  std::vector<paddle::framework::Channel<T>>().swap(new_channels);
  std::vector<paddle::framework::Channel<T>>().swap(new_other_channels);

  new_pv_channels.clear();
  new_other_pv_channels.clear();
  std::vector<paddle::framework::Channel<PvInstance>>().swap(new_pv_channels);
  std::vector<paddle::framework::Channel<PvInstance>>().swap(
      new_other_pv_channels);

  local_vec.clear();
  std::vector<T>().swap(local_vec);
  VLOG(3) << "adjust channel num done";
}

template <typename T>
void DatasetImpl<T>::DynamicAdjustReadersNum(int thread_num) {
  if (thread_num_ == thread_num) {
    VLOG(3) << "DatasetImpl<T>::DynamicAdjustReadersNum thread_num_="
            << thread_num_ << ", thread_num_=thread_num, no need to adjust";
    return;
  }
  VLOG(3) << "adjust readers num from " << thread_num_ << " to " << thread_num;
  thread_num_ = thread_num;
  std::vector<std::shared_ptr<paddle::framework::DataFeed>>().swap(readers_);
  CreateReaders();
  VLOG(3) << "adjust readers num done";
}

template <typename T>
void DatasetImpl<T>::SetFleetSendSleepSeconds(int seconds) {
  fleet_send_sleep_seconds_ = seconds;
}

template <typename T>
void DatasetImpl<T>::CreateReaders() {
  VLOG(3) << "Calling CreateReaders()";
  VLOG(3) << "thread num in Dataset: " << thread_num_;
  VLOG(3) << "Filelist size in Dataset: " << filelist_.size();
  VLOG(3) << "channel num in Dataset: " << channel_num_;
  CHECK(thread_num_ > 0) << "thread num should > 0";
  CHECK(channel_num_ > 0) << "channel num should > 0";
  CHECK(channel_num_ <= thread_num_) << "channel num should <= thread num";
  VLOG(3) << "readers size: " << readers_.size();
  if (readers_.size() != 0) {
    VLOG(3) << "readers_.size() = " << readers_.size()
            << ", will not create again";
    return;
  }
  VLOG(3) << "data feed class name: " << data_feed_desc_.name();
  int channel_idx = 0;
  for (int i = 0; i < thread_num_; ++i) {
    readers_.push_back(DataFeedFactory::CreateDataFeed(data_feed_desc_.name()));
    readers_[i]->Init(data_feed_desc_);
    readers_[i]->SetThreadId(i);
    readers_[i]->SetThreadNum(thread_num_);
    readers_[i]->SetFileListMutex(&mutex_for_pick_file_);
    readers_[i]->SetFileListIndex(&file_idx_);
    readers_[i]->SetFeaNumMutex(&mutex_for_fea_num_);
    readers_[i]->SetFeaNum(&total_fea_num_);
    readers_[i]->SetFileList(filelist_);
    readers_[i]->SetParseInsId(parse_ins_id_);
    readers_[i]->SetParseContent(parse_content_);
    readers_[i]->SetParseLogKey(parse_logkey_);
    readers_[i]->SetEnablePvMerge(enable_pv_merge_);
    // Notice: it is only valid for untest of test_paddlebox_datafeed.
    // In fact, it does not affect the train process when paddle is
    // complied with Box_Ps.
    readers_[i]->SetCurrentPhase(current_phase_);
    if (input_channel_ != nullptr) {
      readers_[i]->SetInputChannel(input_channel_.get());
      readers_[i]->SetInputPtrChannel(input_ptr_channel_.get());
    }
    if (input_pv_channel_ != nullptr) {
      readers_[i]->SetInputPvChannel(input_pv_channel_.get());
    }
    if (cur_channel_ == 0 &&
        static_cast<size_t>(channel_idx) < multi_output_channel_.size()) {
      readers_[i]->SetOutputChannel(multi_output_channel_[channel_idx].get());
      readers_[i]->SetConsumeChannel(multi_consume_channel_[channel_idx].get());
      readers_[i]->SetOutputPtrChannel(output_ptr_channel_[channel_idx].get());
      readers_[i]->SetConsumePtrChannel(
          consume_ptr_channel_[channel_idx].get());
      readers_[i]->SetOutputPvChannel(multi_pv_output_[channel_idx].get());
      readers_[i]->SetConsumePvChannel(multi_pv_consume_[channel_idx].get());
    } else if (static_cast<size_t>(channel_idx) <
               multi_output_channel_.size()) {
      readers_[i]->SetOutputChannel(multi_consume_channel_[channel_idx].get());
      readers_[i]->SetConsumeChannel(multi_output_channel_[channel_idx].get());
      readers_[i]->SetOutputPtrChannel(consume_ptr_channel_[channel_idx].get());
      readers_[i]->SetConsumePtrChannel(output_ptr_channel_[channel_idx].get());
      readers_[i]->SetOutputPvChannel(multi_pv_consume_[channel_idx].get());
      readers_[i]->SetConsumePvChannel(multi_pv_output_[channel_idx].get());
    }
    ++channel_idx;
    if (channel_idx >= channel_num_) {
      channel_idx = 0;
    }
  }
  VLOG(3) << "readers size: " << readers_.size();
}

template <typename T>
void DatasetImpl<T>::DestroyReaders() {
  VLOG(3) << "Calling DestroyReaders()";
  VLOG(3) << "readers size1: " << readers_.size();
  std::vector<std::shared_ptr<paddle::framework::DataFeed>>().swap(readers_);
  VLOG(3) << "readers size: " << readers_.size();
  file_idx_ = 0;
  cur_channel_ = 1 - cur_channel_;
}

template <typename T>
void DatasetImpl<T>::SetPreLoadThreadNum(int thread_num) {
  preload_thread_num_ = thread_num;
}

template <typename T>
void DatasetImpl<T>::CreatePreLoadReaders() {
  VLOG(3) << "Begin CreatePreLoadReaders";
  if (preload_thread_num_ == 0) {
    preload_thread_num_ = thread_num_;
  }
  CHECK(preload_thread_num_ > 0) << "thread num should > 0";
  CHECK(input_channel_ != nullptr);
  preload_readers_.clear();
  for (int i = 0; i < preload_thread_num_; ++i) {
    preload_readers_.push_back(
        DataFeedFactory::CreateDataFeed(data_feed_desc_.name()));
    preload_readers_[i]->Init(data_feed_desc_);
    preload_readers_[i]->SetThreadId(i);
    preload_readers_[i]->SetThreadNum(preload_thread_num_);
    preload_readers_[i]->SetFileListMutex(&mutex_for_pick_file_);
    preload_readers_[i]->SetFileListIndex(&file_idx_);
    preload_readers_[i]->SetFileList(filelist_);
    preload_readers_[i]->SetFeaNumMutex(&mutex_for_fea_num_);
    preload_readers_[i]->SetFeaNum(&total_fea_num_);
    preload_readers_[i]->SetParseInsId(parse_ins_id_);
    preload_readers_[i]->SetParseContent(parse_content_);
    preload_readers_[i]->SetParseLogKey(parse_logkey_);
    preload_readers_[i]->SetEnablePvMerge(enable_pv_merge_);
    preload_readers_[i]->SetInputChannel(input_channel_.get());
    preload_readers_[i]->SetOutputChannel(nullptr);
    preload_readers_[i]->SetConsumeChannel(nullptr);
    preload_readers_[i]->SetOutputPvChannel(nullptr);
    preload_readers_[i]->SetConsumePvChannel(nullptr);
  }
  VLOG(3) << "End CreatePreLoadReaders";
}

template <typename T>
void DatasetImpl<T>::DestroyPreLoadReaders() {
  VLOG(3) << "Begin DestroyPreLoadReaders";
  preload_readers_.clear();
  std::vector<std::shared_ptr<paddle::framework::DataFeed>>().swap(
      preload_readers_);
  file_idx_ = 0;
  VLOG(3) << "End DestroyPreLoadReaders";
}

template <typename T>
int64_t DatasetImpl<T>::GetMemoryDataSize() {
  return input_channel_->Size();
}

template <typename T>
int64_t DatasetImpl<T>::GetPvDataSize() {
  if (enable_pv_merge_) {
    return input_pv_channel_->Size();
  } else {
    VLOG(0) << "It does not merge pv..";
    return 0;
  }
}

template <typename T>
int64_t DatasetImpl<T>::GetShuffleDataSize() {
  int64_t sum = 0;
  for (size_t i = 0; i < multi_output_channel_.size(); ++i) {
    sum += multi_output_channel_[i]->Size() + multi_consume_channel_[i]->Size();
  }
  return sum;
}

template <typename T>
int DatasetImpl<T>::ReceiveFromClient(int msg_type, int client_id,
                                      const std::string& msg) {
#ifdef _LINUX
  VLOG(3) << "ReceiveFromClient msg_type=" << msg_type
          << ", client_id=" << client_id << ", msg length=" << msg.length();
  if (msg.length() == 0) {
    return 0;
  }
  paddle::framework::BinaryArchive ar;
  ar.SetReadBuffer(const_cast<char*>(msg.c_str()), msg.length(), nullptr);
  if (ar.Cursor() == ar.Finish()) {
    return 0;
  }
  std::vector<T> data;
  while (ar.Cursor() < ar.Finish()) {
    data.push_back(ar.Get<T>());
  }
  CHECK(ar.Cursor() == ar.Finish());

  auto fleet_ptr = FleetWrapper::GetInstance();
  // not use random because it doesn't perform well here.
  // to make sure each channel get data equally, we just put data to
  // channel one by one.
  // int64_t index = fleet_ptr->LocalRandomEngine()() % channel_num_;
  int64_t index = 0;
  {
    std::unique_lock<std::mutex> lk(global_index_mutex_);
    index = global_index_++;
  }
  index = index % channel_num_;
  VLOG(3) << "ramdom index=" << index;
  multi_output_channel_[index]->Write(std::move(data));

  data.clear();
  data.shrink_to_fit();
#endif
  return 0;
}

// explicit instantiation
template class DatasetImpl<Record>;

void MultiSlotDataset::PostprocessInstance() {
  // divide pv instance, and merge to input_channel_
  if (enable_pv_merge_) {
    auto fleet_ptr = FleetWrapper::GetInstance();
    std::shuffle(input_records_.begin(), input_records_.end(),
                 fleet_ptr->LocalRandomEngine());
    /*
    input_channel_->Open();
    input_channel_->Write(std::move(input_records_));
    for (size_t i = 0; i < multi_pv_consume_.size(); ++i) {
      multi_pv_consume_[i]->Clear();
    }
    input_channel_->Close();
    input_records_.clear();
    input_records_.shrink_to_fit();
    */
    int all_records_num = input_records_.size();
    std::vector<Record*> all_records;
    all_records.reserve(all_records_num);
    for (int index = 0; index < all_records_num; ++index) {
      all_records.push_back(&input_records_[index]);
    }
    input_ptr_channel_->Open();
    input_ptr_channel_->Write(std::move(all_records));
    input_ptr_channel_->Close();
    VLOG(0) << "input_ptr_channel size: " << input_ptr_channel_->Size();
  } else {
    // TODO(hutuxian): this can be removed
    input_ptr_channel_->Open();
    for (size_t i = 0; i < consume_ptr_channel_.size(); ++i) {
      std::vector<Record*> ins_data;
      consume_ptr_channel_[i]->Close();
      consume_ptr_channel_[i]->ReadAll(ins_data);
      input_ptr_channel_->Write(std::move(ins_data));
      ins_data.clear();
      ins_data.shrink_to_fit();
      consume_ptr_channel_[i]->Clear();
    }
    input_ptr_channel_->Close();
    // this->LocalShuffle();
  }
}

void MultiSlotDataset::SetCurrentPhase(int current_phase) {
  current_phase_ = current_phase;
}

void MultiSlotDataset::PreprocessInstance() {
  if (!input_channel_ || input_channel_->Size() == 0) {
    return;
  }
  if (!enable_pv_merge_) {  // means to use Record
    this->LocalShuffle();
  }
  input_channel_->Close();
  input_channel_->ReadAll(input_records_);
  int all_records_num = input_records_.size();
  std::vector<Record*> all_records;
  all_records.reserve(all_records_num);
  for (int index = 0; index < all_records_num; ++index) {
    all_records.push_back(&input_records_[index]);
  }
  if (!enable_pv_merge_) {
    input_ptr_channel_->Open();
    input_ptr_channel_->Write(std::move(all_records));
    input_ptr_channel_->Close();
    VLOG(0) << "input_ptr_channel size: " << input_ptr_channel_->Size();
    return;
  }

  std::sort(all_records.data(), all_records.data() + all_records_num,
            [](const Record* lhs, const Record* rhs) {
              return lhs->search_id < rhs->search_id;
            });

  std::vector<PvInstance> pv_data;
  if (merge_by_sid_) {
    uint64_t last_search_id = 0;
    for (int i = 0; i < all_records_num; ++i) {
      Record* ins = all_records[i];
      if (i == 0 || last_search_id != ins->search_id) {
        PvInstance pv_instance = make_pv_instance();
        pv_instance->merge_instance(ins);
        pv_data.push_back(pv_instance);
        last_search_id = ins->search_id;
        continue;
      }
      pv_data.back()->merge_instance(ins);
    }
  } else {
    for (int i = 0; i < all_records_num; ++i) {
      Record* ins = all_records[i];
      PvInstance pv_instance = make_pv_instance();
      pv_instance->merge_instance(ins);
      pv_data.push_back(pv_instance);
    }
  }

  auto fleet_ptr = FleetWrapper::GetInstance();
  std::shuffle(pv_data.begin(), pv_data.end(), fleet_ptr->LocalRandomEngine());
  input_pv_channel_->Open();
  input_pv_channel_->Write(std::move(pv_data));

  pv_data.clear();
  pv_data.shrink_to_fit();
  input_pv_channel_->Close();
}

void MultiSlotDataset::GenerateLocalTablesUnlock(int table_id, int feadim,
                                                 int read_thread_num,
                                                 int consume_thread_num,
                                                 int shard_num) {
  VLOG(3) << "MultiSlotDataset::GenerateUniqueFeasign begin";
  if (!gen_uni_feasigns_) {
    VLOG(3) << "generate_unique_feasign_=false, will not GenerateUniqueFeasign";
    return;
  }

  CHECK(multi_output_channel_.size() != 0);  // NOLINT
  auto fleet_ptr_ = FleetWrapper::GetInstance();
  std::vector<std::unordered_map<uint64_t, std::vector<float>>>&
      local_map_tables = fleet_ptr_->GetLocalTable();
  local_map_tables.resize(shard_num);
  // read thread
  int channel_num = multi_output_channel_.size();
  if (read_thread_num < channel_num) {
    read_thread_num = channel_num;
  }
  std::vector<std::thread> threads(read_thread_num);
  consume_task_pool_.resize(consume_thread_num);
  for (size_t i = 0; i < consume_task_pool_.size(); i++) {
    consume_task_pool_[i].reset(new paddle::framework::ThreadPool(1));
  }
  auto gen_func = [this, &shard_num, &feadim, &local_map_tables](int i) {
    std::vector<Record> vec_data;
    std::vector<std::vector<uint64_t>> task_keys(shard_num);
    this->multi_output_channel_[i]->Close();
    this->multi_output_channel_[i]->ReadAll(vec_data);
    for (size_t j = 0; j < vec_data.size(); j++) {
      for (auto& feature : vec_data[j].uint64_feasigns_) {
        int shard = feature.sign().uint64_feasign_ % shard_num;
        task_keys[shard].push_back(feature.sign().uint64_feasign_);
      }
    }

    std::vector<std::future<void>> task_futures;
    for (int shard_id = 0; shard_id < shard_num; shard_id++) {
      auto& keys = task_keys[shard_id];
      task_futures.emplace_back(consume_task_pool_[shard_id]->Run(
          [this, &local_map_tables, shard_id, feadim, &keys]() {
            for (auto k : keys) {
              if (local_map_tables[shard_id].find(k) ==
                  local_map_tables[shard_id].end()) {
                local_map_tables[shard_id][k] = std::vector<float>(feadim, 0);
              }
            }
          }));
    }

    multi_output_channel_[i]->Open();
    multi_output_channel_[i]->Write(std::move(vec_data));
    vec_data.clear();
    vec_data.shrink_to_fit();
    for (auto& tk : task_keys) {
      tk.clear();
      std::vector<uint64_t>().swap(tk);
    }
    task_keys.clear();
    std::vector<std::vector<uint64_t>>().swap(task_keys);
    for (auto& tf : task_futures) {
      tf.wait();
    }
  };
  for (size_t i = 0; i < threads.size(); i++) {
    threads[i] = std::thread(gen_func, i);
  }
  for (std::thread& t : threads) {
    t.join();
  }
  for (size_t i = 0; i < consume_task_pool_.size(); i++) {
    consume_task_pool_[i].reset();
  }
  consume_task_pool_.clear();
  fleet_ptr_->PullSparseToLocal(table_id, feadim);
}

void MultiSlotDataset::MergeByInsId() {
  VLOG(3) << "MultiSlotDataset::MergeByInsId begin";
  if (!merge_by_insid_) {
    VLOG(3) << "merge_by_insid=false, will not MergeByInsId";
    return;
  }
  auto multi_slot_desc = data_feed_desc_.multi_slot_desc();
  std::vector<std::string> use_slots;
  std::vector<bool> use_slots_is_dense;
  for (int i = 0; i < multi_slot_desc.slots_size(); ++i) {
    const auto& slot = multi_slot_desc.slots(i);
    if (slot.is_used()) {
      use_slots.push_back(slot.name());
      use_slots_is_dense.push_back(slot.is_dense());
    }
  }
  CHECK(multi_output_channel_.size() != 0);  // NOLINT
  auto channel_data = paddle::framework::MakeChannel<Record>();
  VLOG(3) << "multi_output_channel_.size() " << multi_output_channel_.size();
  for (size_t i = 0; i < multi_output_channel_.size(); ++i) {
    std::vector<Record> vec_data;
    multi_output_channel_[i]->Close();
    multi_output_channel_[i]->ReadAll(vec_data);
    channel_data->Write(std::move(vec_data));
    vec_data.clear();
    vec_data.shrink_to_fit();
    multi_output_channel_[i]->Clear();
  }
  channel_data->Close();
  std::vector<Record> recs;
  recs.reserve(channel_data->Size());
  channel_data->ReadAll(recs);
  channel_data->Clear();
  std::sort(recs.begin(), recs.end(), [](const Record& a, const Record& b) {
    return a.ins_id_ < b.ins_id_;
  });

  std::vector<Record> results;
  uint64_t drop_ins_num = 0;
  std::unordered_set<uint16_t> all_int64;
  std::unordered_set<uint16_t> all_float;
  std::unordered_set<uint16_t> local_uint64;
  std::unordered_set<uint16_t> local_float;
  std::unordered_map<uint16_t, std::vector<FeatureItem>> all_dense_uint64;
  std::unordered_map<uint16_t, std::vector<FeatureItem>> all_dense_float;
  std::unordered_map<uint16_t, std::vector<FeatureItem>> local_dense_uint64;
  std::unordered_map<uint16_t, std::vector<FeatureItem>> local_dense_float;
  std::unordered_map<uint16_t, bool> dense_empty;

  VLOG(3) << "recs.size() " << recs.size();
  for (size_t i = 0; i < recs.size();) {
    size_t j = i + 1;
    while (j < recs.size() && recs[j].ins_id_ == recs[i].ins_id_) {
      j++;
    }
    if (merge_size_ > 0 && j - i != merge_size_) {
      drop_ins_num += j - i;
      LOG(WARNING) << "drop ins " << recs[i].ins_id_ << " size=" << j - i
                   << ", because merge_size=" << merge_size_;
      i = j;
      continue;
    }

    all_int64.clear();
    all_float.clear();
    all_dense_uint64.clear();
    all_dense_float.clear();
    bool has_conflict_slot = false;
    uint16_t conflict_slot = 0;

    Record rec;
    rec.ins_id_ = recs[i].ins_id_;
    rec.content_ = recs[i].content_;

    for (size_t k = i; k < j; k++) {
      dense_empty.clear();
      local_dense_uint64.clear();
      local_dense_float.clear();
      for (auto& feature : recs[k].uint64_feasigns_) {
        uint16_t slot = feature.slot();
        if (!use_slots_is_dense[slot]) {
          continue;
        }
        local_dense_uint64[slot].push_back(feature);
        if (feature.sign().uint64_feasign_ != 0) {
          dense_empty[slot] = false;
        } else if (dense_empty.find(slot) == dense_empty.end() &&
                   all_dense_uint64.find(slot) == all_dense_uint64.end()) {
          dense_empty[slot] = true;
        }
      }
      for (auto& feature : recs[k].float_feasigns_) {
        uint16_t slot = feature.slot();
        if (!use_slots_is_dense[slot]) {
          continue;
        }
        local_dense_float[slot].push_back(feature);
        if (fabs(feature.sign().float_feasign_) >= 1e-6) {
          dense_empty[slot] = false;
        } else if (dense_empty.find(slot) == dense_empty.end() &&
                   all_dense_float.find(slot) == all_dense_float.end()) {
          dense_empty[slot] = true;
        }
      }
      for (auto& p : dense_empty) {
        if (local_dense_uint64.find(p.first) != local_dense_uint64.end()) {
          all_dense_uint64[p.first] = std::move(local_dense_uint64[p.first]);
        } else if (local_dense_float.find(p.first) != local_dense_float.end()) {
          all_dense_float[p.first] = std::move(local_dense_float[p.first]);
        }
      }
    }
    for (auto& f : all_dense_uint64) {
      rec.uint64_feasigns_.insert(rec.uint64_feasigns_.end(), f.second.begin(),
                                  f.second.end());
    }
    for (auto& f : all_dense_float) {
      rec.float_feasigns_.insert(rec.float_feasigns_.end(), f.second.begin(),
                                 f.second.end());
    }

    for (size_t k = i; k < j; k++) {
      local_uint64.clear();
      local_float.clear();
      for (auto& feature : recs[k].uint64_feasigns_) {
        uint16_t slot = feature.slot();
        if (use_slots_is_dense[slot]) {
          continue;
        } else if (all_int64.find(slot) != all_int64.end()) {
          has_conflict_slot = true;
          conflict_slot = slot;
          break;
        }
        local_uint64.insert(slot);
        rec.uint64_feasigns_.push_back(std::move(feature));
      }
      if (has_conflict_slot) {
        break;
      }
      all_int64.insert(local_uint64.begin(), local_uint64.end());

      for (auto& feature : recs[k].float_feasigns_) {
        uint16_t slot = feature.slot();
        if (use_slots_is_dense[slot]) {
          continue;
        } else if (all_float.find(slot) != all_float.end()) {
          has_conflict_slot = true;
          conflict_slot = slot;
          break;
        }
        local_float.insert(slot);
        rec.float_feasigns_.push_back(std::move(feature));
      }
      if (has_conflict_slot) {
        break;
      }
      all_float.insert(local_float.begin(), local_float.end());
    }

    if (has_conflict_slot) {
      LOG(WARNING) << "drop ins " << recs[i].ins_id_ << " size=" << j - i
                   << ", because conflict_slot=" << use_slots[conflict_slot];
      drop_ins_num += j - i;
    } else {
      results.push_back(std::move(rec));
    }
    i = j;
  }
  std::vector<Record>().swap(recs);
  VLOG(3) << "results size " << results.size();
  LOG(WARNING) << "total drop ins num: " << drop_ins_num;
  results.shrink_to_fit();

  auto fleet_ptr = FleetWrapper::GetInstance();
  std::shuffle(results.begin(), results.end(), fleet_ptr->LocalRandomEngine());
  channel_data->Open();
  channel_data->Write(std::move(results));
  channel_data->Close();
  results.clear();
  results.shrink_to_fit();
  VLOG(3) << "channel data size " << channel_data->Size();
  channel_data->SetBlockSize(channel_data->Size() / channel_num_ + 1);
  VLOG(3) << "channel data block size " << channel_data->BlockSize();
  for (size_t i = 0; i < multi_output_channel_.size(); ++i) {
    std::vector<Record> vec_data;
    channel_data->Read(vec_data);
    multi_output_channel_[i]->Open();
    multi_output_channel_[i]->Write(std::move(vec_data));
    vec_data.clear();
    vec_data.shrink_to_fit();
  }
  CHECK(channel_data->Size() == 0);  // NOLINT
  channel_data->Clear();
  VLOG(3) << "MultiSlotDataset::MergeByInsId end";
}

void MultiSlotDataset::GetRandomData(
    const std::unordered_set<uint16_t>& slots_to_replace,
    std::vector<Record>* result) {
  int debug_erase_cnt = 0;
  int debug_push_cnt = 0;
  auto multi_slot_desc = data_feed_desc_.multi_slot_desc();
  slots_shuffle_rclist_.ReInit();
  const auto& slots_shuffle_original_data = GetSlotsOriginalData();
  for (const auto& rec : slots_shuffle_original_data) {
    RecordCandidate rand_rec;
    Record new_rec = rec;
    slots_shuffle_rclist_.AddAndGet(rec, &rand_rec);
    for (auto it = new_rec.uint64_feasigns_.begin();
         it != new_rec.uint64_feasigns_.end();) {
      if (slots_to_replace.find(it->slot()) != slots_to_replace.end()) {
        it = new_rec.uint64_feasigns_.erase(it);
        debug_erase_cnt += 1;
      } else {
        ++it;
      }
    }
    for (auto slot : slots_to_replace) {
      auto range = rand_rec.feas_.equal_range(slot);
      for (auto it = range.first; it != range.second; ++it) {
        new_rec.uint64_feasigns_.push_back({it->second, it->first});
        debug_push_cnt += 1;
      }
    }
    result->push_back(std::move(new_rec));
  }
  VLOG(2) << "erase feasign num: " << debug_erase_cnt
          << " repush feasign num: " << debug_push_cnt;
}

void MultiSlotDataset::PreprocessChannel(
    const std::set<std::string>& slots_to_replace,
    std::unordered_set<uint16_t>& index_slots) {  // NOLINT
  int out_channel_size = 0;
  if (cur_channel_ == 0) {
    for (size_t i = 0; i < multi_output_channel_.size(); ++i) {
      out_channel_size += multi_output_channel_[i]->Size();
    }
  } else {
    for (size_t i = 0; i < multi_consume_channel_.size(); ++i) {
      out_channel_size += multi_consume_channel_[i]->Size();
    }
  }
  VLOG(2) << "DatasetImpl<T>::SlotsShuffle() begin with input channel size: "
          << input_channel_->Size()
          << " output channel size: " << out_channel_size;

  if ((!input_channel_ || input_channel_->Size() == 0) &&
      slots_shuffle_original_data_.size() == 0 && out_channel_size == 0) {
    VLOG(3) << "DatasetImpl<T>::SlotsShuffle() end, no data to slots shuffle";
    return;
  }

  auto multi_slot_desc = data_feed_desc_.multi_slot_desc();
  for (int i = 0; i < multi_slot_desc.slots_size(); ++i) {
    std::string cur_slot = multi_slot_desc.slots(i).name();
    if (slots_to_replace.find(cur_slot) != slots_to_replace.end()) {
      index_slots.insert(i);
    }
  }
  if (slots_shuffle_original_data_.size() == 0) {
    // before first slots shuffle, instances could be in
    // input_channel, oupput_channel or consume_channel
    if (input_channel_ && input_channel_->Size() != 0) {
      slots_shuffle_original_data_.reserve(input_channel_->Size());
      input_channel_->Close();
      input_channel_->ReadAll(slots_shuffle_original_data_);
    } else {
      CHECK(out_channel_size > 0);  // NOLINT
      if (cur_channel_ == 0) {
        for (size_t i = 0; i < multi_output_channel_.size(); ++i) {
          std::vector<Record> vec_data;
          multi_output_channel_[i]->Close();
          multi_output_channel_[i]->ReadAll(vec_data);
          slots_shuffle_original_data_.reserve(
              slots_shuffle_original_data_.size() + vec_data.size());
          slots_shuffle_original_data_.insert(
              slots_shuffle_original_data_.end(),
              std::make_move_iterator(vec_data.begin()),
              std::make_move_iterator(vec_data.end()));
          vec_data.clear();
          vec_data.shrink_to_fit();
          multi_output_channel_[i]->Clear();
        }
      } else {
        for (size_t i = 0; i < multi_consume_channel_.size(); ++i) {
          std::vector<Record> vec_data;
          multi_consume_channel_[i]->Close();
          multi_consume_channel_[i]->ReadAll(vec_data);
          slots_shuffle_original_data_.reserve(
              slots_shuffle_original_data_.size() + vec_data.size());
          slots_shuffle_original_data_.insert(
              slots_shuffle_original_data_.end(),
              std::make_move_iterator(vec_data.begin()),
              std::make_move_iterator(vec_data.end()));
          vec_data.clear();
          vec_data.shrink_to_fit();
          multi_consume_channel_[i]->Clear();
        }
      }
    }
  } else {
    // if already have original data for slots shuffle, clear channel
    input_channel_->Clear();
    if (cur_channel_ == 0) {
      for (size_t i = 0; i < multi_output_channel_.size(); ++i) {
        if (!multi_output_channel_[i]) {
          continue;
        }
        multi_output_channel_[i]->Clear();
      }
    } else {
      for (size_t i = 0; i < multi_consume_channel_.size(); ++i) {
        if (!multi_consume_channel_[i]) {
          continue;
        }
        multi_consume_channel_[i]->Clear();
      }
    }
    for (size_t i = 0; i < multi_pv_output_.size(); ++i) {
      if (!multi_pv_output_[i]) {
        continue;
      }
      multi_pv_output_[i]->Clear();
    }
    for (size_t i = 0; i < multi_pv_consume_.size(); ++i) {
      if (!multi_pv_consume_[i]) {
        continue;
      }
      multi_pv_consume_[i]->Clear();
    }
  }
  int end_size = 0;
  if (cur_channel_ == 0) {
    for (size_t i = 0; i < multi_output_channel_.size(); ++i) {
      if (!multi_output_channel_[i]) {
        continue;
      }
      end_size += multi_output_channel_[i]->Size();
    }
  } else {
    for (size_t i = 0; i < multi_consume_channel_.size(); ++i) {
      if (!multi_consume_channel_[i]) {
        continue;
      }
      end_size += multi_consume_channel_[i]->Size();
    }
  }
  CHECK(input_channel_->Size() == 0)
      << "input channel should be empty before slots shuffle";
}

// slots shuffle to input_channel_ with needed-shuffle slots
void MultiSlotDataset::SlotsShuffle(
    const std::set<std::string>& slots_to_replace) {
  PADDLE_ENFORCE_EQ(slots_shuffle_fea_eval_, true,
                    platform::errors::PreconditionNotMet(
                        "fea eval mode off, need to set on for slots shuffle"));
  platform::Timer timeline;
  timeline.Start();
  std::unordered_set<uint16_t> index_slots;
  PreprocessChannel(slots_to_replace, index_slots);

  std::vector<Record> random_data;
  random_data.clear();
  // get slots shuffled random_data
  GetRandomData(index_slots, &random_data);
  input_channel_->Open();
  input_channel_->Write(std::move(random_data));
  random_data.clear();
  random_data.shrink_to_fit();
  input_channel_->Close();
  cur_channel_ = 0;

  timeline.Pause();
  VLOG(2) << "DatasetImpl<T>::SlotsShuffle() end"
          << ", memory data size for slots shuffle=" << input_channel_->Size()
          << ", cost time=" << timeline.ElapsedSec() << " seconds";
}

#ifdef PADDLE_WITH_BOX_PS
class PadBoxSlotDataConsumer : public boxps::DataConsumer {
 public:
  explicit PadBoxSlotDataConsumer(PadBoxSlotDataset* dataset)
      : _dataset(dataset), _service_id(-1) {
    _service_id = BoxWrapper::data_shuffle_->register_handler(this);
    CHECK_GE(_service_id, 0);
  }
  virtual ~PadBoxSlotDataConsumer() {
    //    CHECK_GE(BoxWrapper::data_shuffle_->register_handler(this), 0);
    BoxWrapper::data_shuffle_->unregister_consumer(_service_id);
  }
  virtual void on_receive(const int client_id, const char* buff, int len) {
    _dataset->ReceiveSuffleData(client_id, buff, len);
  }

 public:
  void send_message_callback(const int rank_id, const char* buf, int len,
                             boxps::ResultCallback* callback) {
    int client_id = (_service_id << 16) | rank_id;
    BoxWrapper::data_shuffle_->send_message_callback(client_id, buf, len,
                                                     callback);
  }
  void wait_message_done(void) {
    BoxWrapper::data_shuffle_->wait_done(_service_id);
  }

 private:
  PadBoxSlotDataset* _dataset;
  int _service_id;
};
// paddlebox
PadBoxSlotDataset::PadBoxSlotDataset() {
  mpi_size_ = boxps::MPICluster::Ins().size();
  mpi_rank_ = boxps::MPICluster::Ins().rank();
  SlotRecordPool();

  auto boxps_ptr = BoxWrapper::GetInstance();
  int thread_num = boxps_ptr->GetFeedpassThreadNum();
  if (thread_num > FLAGS_padbox_dataset_merge_thread_num) {
    thread_num = FLAGS_padbox_dataset_merge_thread_num;
  }
  merge_thread_num_ = thread_num;
  pass_id_ = boxps_ptr->GetDataSetId();
}
PadBoxSlotDataset::~PadBoxSlotDataset() {}
// create input channel and output channel
void PadBoxSlotDataset::CreateChannel() {
  if (input_channel_ == nullptr) {
    input_channel_ = MakeChannel<SlotRecord>();
    input_channel_->SetBlockSize(OBJPOOL_BLOCK_SIZE);
  }
  if (shuffle_channel_ == nullptr) {
    shuffle_channel_ = MakeChannel<SlotRecord>();
    shuffle_channel_->SetBlockSize(OBJPOOL_BLOCK_SIZE);
  }
}
// set filelist, file_idx_ will reset to zero.
void PadBoxSlotDataset::SetFileList(const std::vector<std::string>& filelist) {
  VLOG(3) << "filelist size: " << filelist.size();
  if (mpi_size_ > 1 && !FLAGS_padbox_dataset_disable_polling) {
    // dualbox
    int num = static_cast<int>(filelist.size());
    for (int i = mpi_rank_; i < num; i = i + mpi_size_) {
      filelist_.push_back(filelist[i]);
    }
  } else {
    filelist_ = filelist;
  }
  file_idx_ = 0;
}
inline paddle::framework::ThreadPool* GetThreadPool(int thread_num) {
  static std::shared_ptr<paddle::framework::ThreadPool> thread_pool = nullptr;
  if (thread_pool == nullptr) {
    thread_pool.reset(new paddle::framework::ThreadPool(thread_num));
  }
  return thread_pool.get();
}
inline paddle::framework::ThreadPool* GetMergePool(int thread_num) {
  static std::shared_ptr<paddle::framework::ThreadPool> thread_pool = nullptr;
  if (thread_pool == nullptr) {
    thread_pool.reset(new paddle::framework::ThreadPool(thread_num));
  }
  return thread_pool.get();
}
inline paddle::framework::ThreadPool* GetShufflePool(int thread_num) {
  static std::shared_ptr<paddle::framework::ThreadPool> thread_pool = nullptr;
  if (thread_pool == nullptr) {
    thread_pool.reset(new paddle::framework::ThreadPool(thread_num));
  }
  return thread_pool.get();
}
void PadBoxSlotDataset::CheckThreadPool(void) {
  wait_futures_.clear();
  if (thread_pool_ != nullptr && merge_pool_ != nullptr) {
    return;
  }
  used_fea_index_.clear();
  auto feed_obj = reinterpret_cast<SlotPaddleBoxDataFeed*>(readers_[0].get());
  feed_obj->GetUsedSlotIndex(&used_fea_index_);

  // read ins thread
  thread_pool_ = GetThreadPool(thread_num_);
  // merge thread
  merge_pool_ = GetMergePool(merge_thread_num_ * 2);
  // shuffle thread
  if (!FLAGS_padbox_dataset_disable_shuffle && mpi_size_ > 1) {
    shuffle_pool_ = GetShufflePool(shuffle_thread_num_ * 2);
  }

  std::vector<int>& cores = boxps::get_readins_cores();
  if (cores.empty()) {
    return;
  }
  thread_pool_->SetCPUAffinity(cores, false);
  merge_pool_->SetCPUAffinity(cores, false);
  if (shuffle_pool_ != nullptr) {
    shuffle_pool_->SetCPUAffinity(cores, false);
  }
}
// pre load
void PadBoxSlotDataset::PreLoadIntoMemory() {
  CheckThreadPool();
  LoadIndexIntoMemory();
  // dualbox global data shuffle
  if (!FLAGS_padbox_dataset_disable_shuffle && mpi_size_ > 1) {
    finished_counter_ = mpi_size_;
    mpi_flags_.assign(mpi_size_, 1);
    VLOG(3) << "RegisterClientToClientMsgHandler";
    data_consumer_ = reinterpret_cast<void*>(new PadBoxSlotDataConsumer(this));
    VLOG(3) << "RegisterClientToClientMsgHandler done";
  }

  read_ins_ref_ = thread_num_;
  for (int64_t i = 0; i < thread_num_; ++i) {
    wait_futures_.emplace_back(thread_pool_->Run([this, i]() {
      platform::Timer timer;
      timer.Start();
      readers_[i]->LoadIntoMemory();
      timer.Pause();
      double span = timer.ElapsedSec();
      if (max_read_ins_span_ < span) {
        max_read_ins_span_ = span;
      }
      if (min_read_ins_span_ == 0 || min_read_ins_span_ > span) {
        min_read_ins_span_ = span;
      }
      if (--read_ins_ref_ == 0) {
        input_channel_->Close();
        other_timer_.Start();
        VLOG(0) << "passid = " << pass_id_
                << ", read ins thread end, max:" << max_read_ins_span_
                << ", min:" << min_read_ins_span_;
      }
    }));
  }

  // dualbox global data shuffle
  if (!FLAGS_padbox_dataset_disable_shuffle && mpi_size_ > 1) {
    ShuffleData(shuffle_thread_num_);
    MergeInsKeys(shuffle_channel_);
  } else {
    MergeInsKeys(input_channel_);
  }
}
void PadBoxSlotDataset::WaitPreLoadDone() {
  for (auto& f : wait_futures_) {
    f.get();
  }
  if (data_consumer_ != nullptr) {
    delete reinterpret_cast<PadBoxSlotDataConsumer*>(data_consumer_);
    data_consumer_ = nullptr;
  }
  if (FLAGS_padbox_dataset_enable_unrollinstance) {
    UnrollInstance();
  }
  VLOG(0) << "passid = " << pass_id_
          << ", PadBoxSlotDataset::WaitPreLoadDone() end"
          << ", memory data size=" << input_records_.size()
          << ", cost time=" << max_read_ins_span_ << " seconds";
}
// load all data into memory
void PadBoxSlotDataset::LoadIntoMemory() {
  VLOG(3) << "DatasetImpl<T>::LoadIntoMemory() begin";
  CheckThreadPool();
  LoadIndexIntoMemory();

  platform::Timer timeline;
  timeline.Start();
  // dualbox global data shuffle
  if (!FLAGS_padbox_dataset_disable_shuffle && mpi_size_ > 1) {
    finished_counter_ = mpi_size_;
    mpi_flags_.assign(mpi_size_, 1);
    VLOG(3) << "RegisterClientToClientMsgHandler";
    data_consumer_ = reinterpret_cast<void*>(new PadBoxSlotDataConsumer(this));
    VLOG(3) << "RegisterClientToClientMsgHandler done";
  }

  read_ins_ref_ = thread_num_;
  for (int64_t i = 0; i < thread_num_; ++i) {
    wait_futures_.emplace_back(thread_pool_->Run([this, i]() {
      readers_[i]->LoadIntoMemory();
      if (--read_ins_ref_ == 0) {
        input_channel_->Close();
      }
    }));
  }

  // dualbox global data shuffle
  if (!FLAGS_padbox_dataset_disable_shuffle && mpi_size_ > 1) {
    ShuffleData(shuffle_thread_num_);
    MergeInsKeys(shuffle_channel_);
  } else {
    MergeInsKeys(input_channel_);
  }
  // wait all thread finish
  for (auto& f : wait_futures_) {
    f.get();
  }

  if (data_consumer_ != nullptr) {
    delete reinterpret_cast<PadBoxSlotDataConsumer*>(data_consumer_);
    data_consumer_ = nullptr;
  }
  if (FLAGS_padbox_dataset_enable_unrollinstance) {
    UnrollInstance();
  }
  timeline.Pause();

  VLOG(1) << "PadBoxSlotDataset::LoadIntoMemory() end"
          << ", memory data size=" << input_records_.size()
          << ", cost time=" << timeline.ElapsedSec() << " seconds";
}
// add fea keys
void PadBoxSlotDataset::MergeInsKeys(const Channel<SlotRecord>& in) {
  merge_ins_ref_ = merge_thread_num_;
  input_records_.clear();
  min_merge_ins_span_ = 1000;
  CHECK(p_agent_ != nullptr);
  for (int tid = 0; tid < merge_thread_num_; ++tid) {
    wait_futures_.emplace_back(merge_pool_->Run([this, &in, tid]() {
      //      VLOG(0) << "merge thread id: " << tid << "start";
      platform::Timer timer;
      auto feed_obj =
          reinterpret_cast<SlotPaddleBoxDataFeed*>(readers_[0].get());
      size_t num = 0;
      std::vector<SlotRecord> datas;
      while (in->ReadOnce(datas, OBJPOOL_BLOCK_SIZE)) {
        timer.Resume();
        for (auto& rec : datas) {
          for (auto& idx : used_fea_index_) {
            uint64_t* feas = rec->slot_uint64_feasigns_.get_values(idx, &num);
            if (num > 0) {
              p_agent_->AddKeys(feas, num, tid);
            }
          }
          feed_obj->ExpandSlotRecord(&rec);
        }

        merge_mutex_.lock();
        for (auto& t : datas) {
          input_records_.push_back(std::move(t));
        }
        merge_mutex_.unlock();
        datas.clear();
        timer.Pause();
      }
      datas.shrink_to_fit();

      double span = timer.ElapsedSec();
      if (max_merge_ins_span_ < span) {
        max_merge_ins_span_ = span;
      }
      if (min_merge_ins_span_ > span) {
        min_merge_ins_span_ = span;
      }
      // end merge thread
      if (--merge_ins_ref_ == 0) {
        other_timer_.Pause();
        VLOG(0) << "passid = " << pass_id_ << ", merge thread id: " << tid
                << ", span time: " << span << ", max:" << max_merge_ins_span_
                << ", min:" << min_merge_ins_span_;
      }
      //      else {
      //          VLOG(0) << "merge thread id: " << tid
      //              << ", span time: " << span;
      //      }
    }));
  }
}
// release all memory data
void PadBoxSlotDataset::ReleaseMemory() {
  VLOG(3) << "DatasetImpl<T>::ReleaseMemory() begin";
  platform::Timer timeline;
  timeline.Start();

  if (input_channel_) {
    input_channel_->Clear();
    input_channel_ = nullptr;
  }

  if (shuffle_channel_) {
    shuffle_channel_->Clear();
    shuffle_channel_ = nullptr;
  }

  readers_.clear();
  readers_.shrink_to_fit();

  SlotRecordPool().put(&input_records_);
  input_records_.clear();
  input_records_.shrink_to_fit();

  if (!input_pv_ins_.empty()) {
    for (auto& pv : input_pv_ins_) {
      delete pv;
    }
    input_pv_ins_.clear();
    input_pv_ins_.shrink_to_fit();
  }
  timeline.Pause();
  VLOG(1) << "DatasetImpl<T>::ReleaseMemory() end, cost time="
          << timeline.ElapsedSec()
          << " seconds, object pool size=" << SlotRecordPool().capacity();
}
class ShuffleResultWaitGroup : public boxps::ResultCallback {
 public:
  ShuffleResultWaitGroup() {}
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    counter_ = 0;
    cond_.notify_all();
  }
  void add(int delta) {
    if (delta == 0) {
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    counter_ += delta;

    if (counter_ == 0) {
      cond_.notify_all();
    }
  }
  void done() { add(-1); }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (counter_ != 0) {
      cond_.wait(lock);
    }
  }

 public:
  virtual void on_notify(void) { done(); }

 private:
  std::mutex mutex_;
  std::condition_variable cond_;
  int counter_ = 0;
};
// shuffle data
void PadBoxSlotDataset::ShuffleData(int thread_num) {
  CHECK_GT(thread_num, 0);
  VLOG(3) << "start global shuffle threads, num = " << thread_num;
  shuffle_counter_ = thread_num;
  min_shuffle_span_ = 1000;
  for (int tid = 0; tid < thread_num; ++tid) {
    wait_futures_.emplace_back(shuffle_pool_->Run([this, tid]() {
      platform::Timer timer;
      std::vector<SlotRecord> data;
      std::vector<SlotRecord> loc_datas;
      std::vector<SlotRecord> releases;
      std::vector<paddle::framework::BinaryArchive> ars(mpi_size_);
      PadBoxSlotDataConsumer* handler =
          reinterpret_cast<PadBoxSlotDataConsumer*>(data_consumer_);
      ShuffleResultWaitGroup wg;
      while (input_channel_->Read(data)) {
        timer.Resume();
        for (auto& t : data) {
          int client_id = 0;
          if (enable_pv_merge_ ||
              FLAGS_enable_shuffle_by_searchid) {  // shuffle by pv
            client_id = t->search_id % mpi_size_;
          } else if (merge_by_insid_) {  // shuffle by lineid
            client_id =
                XXH64(t->ins_id_.data(), t->ins_id_.length(), 0) % mpi_size_;
          } else {  // shuffle
            client_id = BoxWrapper::LocalRandomEngine()() % mpi_size_;
          }
          if (client_id == mpi_rank_) {
            loc_datas.push_back(std::move(t));
            continue;
          }
          ars[client_id] << t;
          releases.push_back(t);
        }
        SlotRecordPool().put(&releases);
        releases.clear();
        size_t loc_len = loc_datas.size();
        CHECK(shuffle_channel_->Write(std::move(loc_datas)) == loc_len);

        wg.wait();
        wg.add(mpi_size_);
        for (int i = 0; i < mpi_size_; ++i) {
          if (i == mpi_rank_) {
            wg.done();
            continue;
          }
          auto& ar = ars[i];
          if (ar.Length() == 0) {
            wg.done();
            continue;
          }
          handler->send_message_callback(i, ar.Buffer(), ar.Length(), &wg);
          ar.Clear();
        }

        data.clear();
        loc_datas.clear();
        timer.Pause();
      }
      timer.Resume();
      wg.wait();
      timer.Pause();

      data.shrink_to_fit();
      loc_datas.shrink_to_fit();
      releases.shrink_to_fit();

      double span = timer.ElapsedSec();
      if (span > max_shuffle_span_) {
        max_shuffle_span_ = span;
      }
      if (span < min_shuffle_span_) {
        min_shuffle_span_ = span;
      }
      VLOG(3) << "passid = " << pass_id_ << ", end shuffle thread id=" << tid
              << ", span: " << span;
      // only one thread send finish notify
      if (--shuffle_counter_ == 0) {
        timer.Start();
        // send closed
        wg.add(mpi_size_);
        for (int i = 0; i < mpi_size_; ++i) {
          if (i == mpi_rank_) {
            wg.done();
            continue;
          }
          handler->send_message_callback(i, NULL, 0, &wg);
        }
        wg.wait();
        // wait message done
        handler->wait_message_done();
        timer.Pause();

        // end shuffle thread
        LOG(WARNING) << "passid = " << pass_id_
                     << ", end shuffle span max:" << max_shuffle_span_
                     << ", min:" << min_shuffle_span_
                     << ", wait:" << timer.ElapsedSec();
        // local closed channel
        if (--finished_counter_ == 0) {
          while (receiver_cnt_ > 0) {
            usleep(100);
          }
          shuffle_channel_->Close();
          LOG(WARNING) << "passid = " << pass_id_
                       << ", ShuffleData rank_id=" << mpi_rank_
                       << " close channel";
        }
      }
    }));
  }
}
void PadBoxSlotDataset::ReceiveSuffleData(int client_id, const char* buf,
                                          int len) {
  ++receiver_cnt_;
  VLOG(3) << "ReceiveFromClient client_id=" << client_id
          << ", msg length=" << len;
  if (len == 0) {
    if (mpi_flags_[client_id]) {
      mpi_flags_[client_id] = 0;
      --finished_counter_;
    }
    --receiver_cnt_;

    if (finished_counter_ == 0) {
      usleep(10000);
      while (receiver_cnt_ > 0) {
        usleep(100);
      }
      shuffle_channel_->Close();
      LOG(WARNING) << "passid = " << pass_id_
                   << ", ReceiveFromClient client_id=" << client_id
                   << " close channel";
    }
    return;
  }

  paddle::framework::BinaryArchive ar;
  ar.SetReadBuffer(const_cast<char*>(buf), len, nullptr);

  static const int max_fetch_num = OBJPOOL_BLOCK_SIZE / mpi_size_;
  int offset = 0;
  std::vector<SlotRecord> data;
  SlotRecordPool().get(&data, max_fetch_num);
  while (ar.Cursor() < ar.Finish()) {
    ar >> data[offset++];
    if (offset >= max_fetch_num) {
      CHECK(shuffle_channel_->Write(std::move(data)) ==
            static_cast<size_t>(offset));
      data.clear();
      offset = 0;
      SlotRecordPool().get(&data, max_fetch_num);
    }
  }
  CHECK(ar.Cursor() == ar.Finish());
  if (offset > 0) {
    CHECK(shuffle_channel_->WriteMove(offset, &data[0]) ==
          static_cast<size_t>(offset));
    if (offset < max_fetch_num) {
      SlotRecordPool().put(&data[offset], (max_fetch_num - offset));
    }
  } else {
    SlotRecordPool().put(&data);
  }

  data.clear();
  data.shrink_to_fit();
  --receiver_cnt_;
}
// create readers
void PadBoxSlotDataset::CreateReaders() {
  VLOG(3) << "Calling CreateReaders()"
          << "thread num in Dataset: " << thread_num_
          << "Filelist size in Dataset: " << filelist_.size()
          << "readers size: " << readers_.size();
  if (readers_.size() != 0) {
    VLOG(3) << "readers_.size() = " << readers_.size()
            << ", will not create again";
    return;
  }
  VLOG(3) << "data feed class name: " << data_feed_desc_.name();
  for (int i = 0; i < thread_num_; ++i) {
    readers_.push_back(DataFeedFactory::CreateDataFeed(data_feed_desc_.name()));
    readers_[i]->Init(data_feed_desc_);
    readers_[i]->SetThreadId(i);
    readers_[i]->SetThreadNum(thread_num_);
    readers_[i]->SetFileListMutex(&mutex_for_pick_file_);
    readers_[i]->SetFileListIndex(&file_idx_);
    readers_[i]->SetFileList(filelist_);
    readers_[i]->SetParseInsId(parse_ins_id_);
    readers_[i]->SetParseContent(parse_content_);
    readers_[i]->SetParseLogKey(parse_logkey_);
    readers_[i]->SetEnablePvMerge(enable_pv_merge_);
    // Notice: it is only valid for untest of test_paddlebox_datafeed.
    // In fact, it does not affect the train process when paddle is
    // complied with Box_Ps.
    readers_[i]->SetCurrentPhase(current_phase_);
    if (input_channel_ != nullptr) {
      readers_[i]->SetInputChannel(input_channel_.get());
    }
  }
  VLOG(3) << "readers size: " << readers_.size();
}
// destroy readers
void PadBoxSlotDataset::DestroyReaders() {
  readers_.clear();
  readers_.shrink_to_fit();
}

// merge pv instance
void PadBoxSlotDataset::PreprocessInstance() {
  if (input_records_.empty()) {
    return;
  }
  if (!enable_pv_merge_) {  // means to use Record
    return;
  }

  if (!input_pv_ins_.empty()) {  // for auc runner
    for (auto pv : input_pv_ins_) {
      delete pv;
    }
    input_pv_ins_.clear();
  }

  size_t all_records_num = input_records_.size();
  std::sort(input_records_.data(), input_records_.data() + all_records_num,
            [](const SlotRecord& lhs, const SlotRecord& rhs) {
              return lhs->search_id < rhs->search_id;
            });
  if (merge_by_sid_) {
    uint64_t last_search_id = 0;
    for (size_t i = 0; i < all_records_num; ++i) {
      auto& ins = input_records_[i];
      if (i == 0 || last_search_id != ins->search_id) {
        SlotPvInstance pv_instance = make_slotpv_instance();
        pv_instance->merge_instance(ins);
        input_pv_ins_.push_back(pv_instance);
        last_search_id = ins->search_id;
        continue;
      }
      input_pv_ins_.back()->merge_instance(ins);
    }
  } else {
    for (size_t i = 0; i < all_records_num; ++i) {
      auto& ins = input_records_[i];
      SlotPvInstance pv_instance = make_slotpv_instance();
      pv_instance->merge_instance(ins);
      input_pv_ins_.push_back(pv_instance);
    }
  }
}
// restore
void PadBoxSlotDataset::PostprocessInstance() {}

/**
 * @Brief
 * Split the remaining data to each thread
 */
static void compute_left_batch_num(const int ins_num, const int thread_num,
                                   std::vector<std::pair<int, int>>* offset,
                                   const int start_pos) {
  int cur_pos = start_pos;
  int batch_size = ins_num / thread_num;
  int left_num = ins_num % thread_num;
  for (int i = 0; i < thread_num; ++i) {
    int batch_num_size = batch_size;
    if (i == 0) {
      batch_num_size = batch_num_size + left_num;
    }
    offset->push_back(std::make_pair(cur_pos, batch_num_size));
    cur_pos += batch_num_size;
  }
}

/**
 * @brief
 * distributed to each thread according to the amount of data
 */
static void compute_batch_num(const int64_t ins_num, const int batch_size,
                              const int thread_num,
                              std::vector<std::pair<int, int>>* offset) {
  int thread_batch_num = batch_size * thread_num;
  // less data
  if (static_cast<int64_t>(thread_batch_num) > ins_num) {
    compute_left_batch_num(ins_num, thread_num, offset, 0);
    return;
  }

  int cur_pos = 0;
  int offset_num = static_cast<int>(ins_num / thread_batch_num) * thread_num;
  int left_ins_num = static_cast<int>(ins_num % thread_batch_num);
  if (left_ins_num > 0 && left_ins_num < (thread_num * 2) && offset_num > 1) {
    offset_num = offset_num - thread_num;
    left_ins_num = left_ins_num + thread_batch_num;
    for (int i = 0; i < offset_num; ++i) {
      offset->push_back(std::make_pair(cur_pos, batch_size));
      cur_pos += batch_size;
    }
    // split data to thread avg two rounds
    compute_left_batch_num(left_ins_num, thread_num * 2, offset, cur_pos);
  } else {
    for (int i = 0; i < offset_num; ++i) {
      offset->push_back(std::make_pair(cur_pos, batch_size));
      cur_pos += batch_size;
    }
    if (left_ins_num > 0) {
      compute_left_batch_num(left_ins_num, thread_num, offset, cur_pos);
    }
  }
}

static int compute_thread_batch_nccl(
    const int thr_num, const int64_t total_instance_num,
    const int minibatch_size, std::vector<std::pair<int, int>>* nccl_offsets) {
  int thread_avg_batch_num = 0;
  if (total_instance_num < static_cast<int64_t>(thr_num)) {
    LOG(WARNING) << "compute_thread_batch_nccl total ins num:["
                 << total_instance_num << "], less thread num:[" << thr_num
                 << "]";
    return thread_avg_batch_num;
  }

  auto& offset = (*nccl_offsets);
  // split data avg by thread num
  compute_batch_num(total_instance_num, minibatch_size, thr_num, &offset);
  thread_avg_batch_num = static_cast<int>(offset.size() / thr_num);

  auto& mpi = boxps::MPICluster::Ins();
  if (mpi.size() > 1) {
    // 这里主要针对NCCL需要相同的minibatch才能正常处理
    int thread_max_batch_num = mpi.allreduce(thread_avg_batch_num, 0);
    int64_t sum_total_ins_num = mpi.allreduce(total_instance_num, 2);
    int diff_batch_num = thread_max_batch_num - thread_avg_batch_num;
    if (diff_batch_num == 0) {
      LOG(WARNING) << "total sum ins " << sum_total_ins_num << ", thread_num "
                   << thr_num << ", ins num " << total_instance_num
                   << ", batch num " << offset.size()
                   << ", thread avg batch num " << thread_avg_batch_num;
      return thread_avg_batch_num;
    }

    int need_ins_num = thread_max_batch_num * thr_num;
    // data is too less
    if ((int64_t)need_ins_num > total_instance_num) {
      LOG(FATAL) << "error instance num:[" << total_instance_num
                 << "] less need ins num:[" << need_ins_num << "]";
      return thread_avg_batch_num;
    }

    int need_batch_num = (diff_batch_num + 1) * thr_num;
    int offset_split_index = static_cast<int>(offset.size() - thr_num);
    int split_left_num = total_instance_num - offset[offset_split_index].first;
    while (split_left_num < need_batch_num) {
      need_batch_num += thr_num;
      offset_split_index -= thr_num;
      split_left_num = total_instance_num - offset[offset_split_index].first;
    }
    int split_start = offset[offset_split_index].first;
    offset.resize(offset_split_index);
    compute_left_batch_num(split_left_num, need_batch_num, &offset,
                           split_start);
    LOG(WARNING) << "total sum ins " << sum_total_ins_num << ", thread_num "
                 << thr_num << ", ins num " << total_instance_num
                 << ", batch num " << offset.size() << ", thread avg batch num "
                 << thread_avg_batch_num << ", thread max batch num "
                 << thread_max_batch_num
                 << ", need batch num: " << (need_batch_num / thr_num)
                 << "split begin (" << split_start << ")" << split_start
                 << ", num " << split_left_num;
    thread_avg_batch_num = thread_max_batch_num;
  } else {
    LOG(WARNING) << "thread_num " << thr_num << ", ins num "
                 << total_instance_num << ", batch num " << offset.size()
                 << ", thread avg batch num " << thread_avg_batch_num;
  }
  return thread_avg_batch_num;
}

// dynamic adjust reader num
void PadBoxSlotDataset::DynamicAdjustReadersNum(int thread_num) {
  if (thread_num_ == thread_num) {
    VLOG(3) << "DatasetImpl<T>::DynamicAdjustReadersNum thread_num_="
            << thread_num_ << ", thread_num_=thread_num, no need to adjust";
    PrepareTrain();
    return;
  }
  VLOG(3) << "adjust readers num from " << thread_num_ << " to " << thread_num;
  thread_num_ = thread_num;
  readers_.clear();
  readers_.shrink_to_fit();
  CreateReaders();
  VLOG(3) << "adjust readers num done";
  PrepareTrain();
}

// prepare train do something
void PadBoxSlotDataset::PrepareTrain(void) {
  auto box_ptr = paddle::framework::BoxWrapper::GetInstance();

  std::vector<std::pair<int, int>> offset;
  // join or aucrunner mode enable pv
  if (enable_pv_merge_ && (box_ptr->Phase() == 1 || box_ptr->Mode() == 1)) {
    std::shuffle(input_pv_ins_.begin(), input_pv_ins_.end(),
                 BoxWrapper::LocalRandomEngine());
    // 分数据到各线程里面
    int batchsize = reinterpret_cast<SlotPaddleBoxDataFeed*>(readers_[0].get())
                        ->GetPvBatchSize();
    compute_thread_batch_nccl(thread_num_, GetPvDataSize(), batchsize, &offset);
    for (int i = 0; i < thread_num_; ++i) {
      reinterpret_cast<SlotPaddleBoxDataFeed*>(readers_[i].get())
          ->SetPvInstance(&input_pv_ins_[0]);
    }
    for (size_t i = 0; i < offset.size(); ++i) {
      reinterpret_cast<SlotPaddleBoxDataFeed*>(readers_[i % thread_num_].get())
          ->AddBatchOffset(offset[i]);
    }
  } else {
    std::shuffle(input_records_.begin(), input_records_.end(),
                 BoxWrapper::LocalRandomEngine());
    // 分数据到各线程里面
    int batchsize = reinterpret_cast<SlotPaddleBoxDataFeed*>(readers_[0].get())
                        ->GetBatchSize();
    compute_thread_batch_nccl(thread_num_, GetMemoryDataSize(), batchsize,
                              &offset);
    for (int i = 0; i < thread_num_; ++i) {
      reinterpret_cast<SlotPaddleBoxDataFeed*>(readers_[i].get())
          ->SetSlotRecord(&input_records_[0]);
    }
    for (size_t i = 0; i < offset.size(); ++i) {
      reinterpret_cast<SlotPaddleBoxDataFeed*>(readers_[i % thread_num_].get())
          ->AddBatchOffset(offset[i]);
    }
  }
}

void PadBoxSlotDataset::UnrollInstance() {
  auto feed_obj = reinterpret_cast<SlotPaddleBoxDataFeed*>(readers_[0].get());
  feed_obj->UnrollInstance(input_records_);
}

void InputTableDataset::LoadIndexIntoMemory() {
  VLOG(3) << "LoadIndexIntoMemory()";
  platform::Timer timer;
  timer.Start();

  std::vector<std::shared_ptr<paddle::framework::DataFeed>> readers;
  size_t file_idx = 0;
  std::mutex mutex_for_pick_file;

  for (int i = 0; i < thread_num_; ++i) {
    readers.push_back(DataFeedFactory::CreateDataFeed("InputIndexDataFeed"));
    readers[i]->Init(data_feed_desc_);
    readers[i]->SetThreadId(i);
    readers[i]->SetFileListMutex(&mutex_for_pick_file);
    readers[i]->SetFileListIndex(&file_idx);
    readers[i]->SetFileList(index_filelist_);
  }

  std::vector<std::future<void>> wait_futures;
  for (int i = 0; i < thread_num_; ++i) {
    wait_futures.emplace_back(
        thread_pool_->Run([i, &readers]() { readers[i]->LoadIntoMemory(); }));
  }
  for (auto& f : wait_futures) {
    f.wait();
  }
  timer.Pause();
  VLOG(1) << "end LoadIndexIntoMemory() cost: " << timer.ElapsedSec();
}

#endif
}  // end namespace framework
}  // end namespace paddle
