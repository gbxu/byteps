// Copyright 2019 ByteDance Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "global.h"
#include <cuda_runtime.h>

namespace byteps {
namespace common {

void BytePSScheduledQueue::addTask(std::shared_ptr<TensorTableEntry> entry) {
    std::lock_guard<std::mutex> lock(_mutex);
    _sq.push_back(entry);
    return;
}

std::shared_ptr<TensorTableEntry> BytePSScheduledQueue::getTask() {
    std::lock_guard<std::mutex> lock(_mutex);
    auto front = _sq.front();
    _sq.pop_front();
    return front;
}

std::shared_ptr<TensorTableEntry> BytePSScheduledQueue::peekTask() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _sq.front();
}

uint32_t BytePSScheduledQueue::pendingSize() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _sq.size();
}

void BytePSScheduledQueue::reportFinish(std::shared_ptr<TensorTableEntry> e) {
    // TODO: return credit based on TensorTableEntry
    _finished++;
    return;
}

// Define and init global variables
std::mutex BytePSGlobal::_init_mutex;
volatile bool BytePSGlobal::_initialized = false;
volatile bool BytePSGlobal::_should_shutdown = false;

int BytePSGlobal::_rank = 0;
int BytePSGlobal::_local_rank = 0;
int BytePSGlobal::_size = 1;
int BytePSGlobal::_local_size = 1;
uint32_t BytePSGlobal::_partition_bound = 512000;
std::shared_ptr<BytePSComm> BytePSGlobal::_comm;
std::unordered_map<int, PSKV> BytePSGlobal::ps_kv_;

volatile BytePSScheduledQueue* BytePSGlobal::_queues[QueueNum] = {NULL};
std::mutex BytePSGlobal::_queues_mutex[QueueNum];
std::thread* BytePSGlobal::_threads[QueueNum] = {NULL};

ps::KVWorker<char>* BytePSGlobal::_ps = NULL;
std::mutex BytePSGlobal::_encode_mutex;
std::unordered_map<std::string, BPSContext> BytePSGlobal::_name_to_cxt;
unsigned int next_key_ = 0;
cudaStream_t* BytePSGlobal::_reduce_stream;
cudaStream_t* BytePSGlobal::_broadcast_stream;

BytePSScheduledQueue* BytePSGlobal::GetScheduledQueue(QueueType queueType) {
    return (BytePSScheduledQueue*)_queues[queueType];
}

void* BytePSGlobal::CreateScheduledQueue(QueueType queueType) {
    std::lock_guard<std::mutex> lock(_queues_mutex[queueType]);
    if (!_queues[queueType]) {
        _queues[queueType] = new BytePSScheduledQueue();
    }
}

void BytePSGlobal::Init() {
    std::lock_guard<std::mutex> lock(_init_mutex);
    
    // We only init once
    if (_initialized) {
        return;
    }

#ifdef BYTEPS_USE_MPI
    _comm = std::make_shared<BytePSCommMPI>();
#else
    _comm = std::make_shared<BytePSCommSocket>();
#endif // BYTEPS_USE_MPI

    _comm->init(&_rank, &_size, &_local_rank, &_local_size);

    if (getenv("BYTEPS_PARTITION_BOUND")) _partition_bound = atoi(getenv("BYTEPS_PARTITION_BOUND"));
    BPS_LOG(DEBUG) << "Partition bound set to " << _partition_bound << " (parameters)";

    // init low-level ps implementation
    _ps = new ps::KVWorker<char>(0, 0);
    ps::StartAsync(0, "byteps\0");
    if (!ps::Postoffice::Get()->is_recovery()) {
        ps::Postoffice::Get()->Barrier(
            0, ps::kWorkerGroup + ps::kServerGroup + ps::kScheduler);
    }

    _reduce_stream = (cudaStream_t*) malloc(sizeof(cudaStream_t) * 1);
    _broadcast_stream = (cudaStream_t*) malloc(sizeof(cudaStream_t) * 1);
    cudaStreamCreateWithFlags(_reduce_stream, cudaStreamNonBlocking);
    cudaStreamCreateWithFlags(_broadcast_stream, cudaStreamNonBlocking);

    for (int i = 0; i < QueueNum; i++) {
        BPS_LOG(DEBUG) << "Create schedule queue " << i;
        auto type = static_cast<QueueType>(i);
        BytePSGlobal::CreateScheduledQueue(type);
    }

    _initialized = true;
    BPS_LOG(DEBUG) << "Inited rank=" << _rank << " local_rank=" << _local_rank
               << " size=" << _size << " local_size=" << _local_size;
    return;
}

void BytePSGlobal::Start(LoopFunction* func) {
    // Start background threads
    for (int i = 0; i < ThreadNum; i++) {
        _threads[i] = new std::thread(func[i]);
        BPS_LOG(DEBUG) << "Background thread " << i << " starts.";
    }
}


const Status NOT_INITIALIZED_ERROR = Status::PreconditionError(
    "BytePS has not been initialized; use bps.init().");

Status BytePSGlobal::CheckInit() {
    if (_initialized) {
        return Status::OK();
    }
    else {
        return NOT_INITIALIZED_ERROR;
    }
}

void BytePSGlobal::Shutdown() {
    _should_shutdown = true;
    for (int i = 0; i < ThreadNum; i++) {
        if (_threads[i]->joinable()) {
            _threads[i]->join();
            delete _threads[i];
        }
    }
    ps::Finalize(0, true);

    cudaStreamDestroy(*_reduce_stream);
    cudaStreamDestroy(*_broadcast_stream);

    for (auto &it:_name_to_cxt) {
        CUDA_CALL(cudaFreeHost(it.second.cpubuff));
    }
    return;
}

BPSContext& BytePSGlobal::GetContextFromName(const std::string &name) {
    std::lock_guard<std::mutex> lock(_encode_mutex);
    return _name_to_cxt[name];
}

void BytePSGlobal::ConvertBoundToBytes(DataType dtype, uint32_t& bound) {
    int byte_per_param;
    switch (dtype) {
        case DataType::BYTEPS_UINT8:
        case DataType::BYTEPS_INT8:
            byte_per_param = 1;
            break;
        case DataType::BYTEPS_FLOAT16:
            byte_per_param = 2;
            break;
        case DataType::BYTEPS_FLOAT32:
        case DataType::BYTEPS_INT32:
            byte_per_param = 4;
            break;
        case DataType::BYTEPS_FLOAT64:
        case DataType::BYTEPS_INT64:
            byte_per_param = 8;
            break;
    }
    BPS_LOG(DEBUG) << "The partition bound is " << bound
                   << " params (or "
                   << (bound * byte_per_param) << " Bytes)";
    bound *= byte_per_param;
}

bool BytePSGlobal::IsTensorInitialized(const std::string &name, size_t size, int device, DataType dtype) {
    std::lock_guard<std::mutex> lock(_encode_mutex);
    BPS_CHECK_GT(size, 0) << "init tensor size not larger than 0, should check this";

    if (_name_to_cxt.find(name) == _name_to_cxt.end()) {
        if (next_key_ == 0) { // only do this once
            ConvertBoundToBytes(dtype, _partition_bound);
        }

        if (device != CPU_DEVICE_ID) { // GPU
            //BPS_LOG(TRACE) << name << ": Init the associated CPU buffer with len=" << size;
            CUDA_CALL(cudaHostAlloc((void **) &_name_to_cxt[name].cpubuff, size, cudaHostAllocMapped));
            _name_to_cxt[name].buff_len = size;
        }
        auto accumulated = 0;
        while (accumulated < size) {
            _name_to_cxt[name].key_list.push_back((ps::Key) next_key_++);
            accumulated += ((size - accumulated) > _partition_bound) ? _partition_bound : (size - accumulated);
        }
        BPS_LOG(DEBUG) << name << " partitioned to "
                       << _name_to_cxt[name].key_list.size() << " part(s)"
                       << ", total_len=" << size
                       << ", key_range=["
                       << _name_to_cxt[name].key_list.front()
                       << ", "
                       << _name_to_cxt[name].key_list.back()
                       << "]";
        return false;
    }
    return true;
}

PSKV& BytePSGlobal::EncodeDefaultKey(int key, size_t len) {
    _encode_mutex.lock();
    PSKV& pskv = ps_kv_[key];
    _encode_mutex.unlock();
    if (!pskv.keys.empty()) {
        BPS_CHECK_EQ(static_cast<size_t>(pskv.size), len)
            << "The value size cannot be changed " << len
            << ". Key is " << key;
    } else {
        auto krs = ps::Postoffice::Get()->GetServerKeyRanges();
        const int num_servers = krs.size();
        BPS_CHECK_GT(num_servers, 0);
        // send it to a single random picked server
        int server = (key * 9973) % num_servers;
        BPS_LOG(DEBUG) << "key " << key << " assigned to server " << server;
        ps::Key ps_key = krs[server].begin() + key;
        BPS_CHECK_LT(ps_key, krs[server].end());
        pskv.keys.push_back(ps_key);
        pskv.lens.push_back(len);
        pskv.size = len;
    }
    return pskv;
}

uint32_t BytePSGlobal::GetTensorCount() {
    std::lock_guard<std::mutex> lock(_encode_mutex);
    return BytePSGlobal::_name_to_cxt.size();
}

cudaStream_t* BytePSGlobal::GetReduceStream() {
    return BytePSGlobal::_reduce_stream;
}

cudaStream_t* BytePSGlobal::GetBroadcastStream() {
    return BytePSGlobal::_broadcast_stream;
}

} // namespace common
} // namespace byteps
