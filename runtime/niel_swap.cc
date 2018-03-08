#include "niel_swap.h"

#include "gc/heap.h"
#include "gc/task_processor.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "runtime.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <pthread.h>
#include <set>
#include <vector>

namespace art {

namespace niel {

namespace swap {

/* Constants */
const int MAGIC_NUM = 42424242;
const double COMPACT_THRESHOLD = 0.25;

/* Internal functions */
std::string getPackageName();
void openFile(const std::string & path, std::fstream & stream);
bool checkStreamError(const std::ios & stream, const std::string & msg);
void validateSwapFile();

/* Variables */
Mutex writeQueueMutex("WriteQueueMutex", kLoggingLock);
pthread_mutex_t objectLock = PTHREAD_MUTEX_INITIALIZER;

// Not locked but assumed to be only touched in one thread (because all Tasks,
// including GC, run on the same fixed thread)
std::fstream swapfile;
uint32_t pid = 0;
std::map<void *, std::streampos> objectOffsetMap;
std::map<void *, size_t> objectSizeMap;
int freedObjects = 0;
long freedSize = 0;
int swapfileObjects = 0;
int swapfileSize = 0;

// Locked by writeQueueMutex
std::vector<mirror::Object *> writeQueue;
std::set<mirror::Object *> writeSet; // prevent duplicate entries in writeQueue

class WriteTask : public gc::HeapTask {
  public:
    WriteTask(uint64_t target_time) : gc::HeapTask(target_time) { }

    virtual void Run(Thread * self) {
        bool done = false;
        uint64_t startTime = NanoTime();
        bool ioError = false;

        int numGarbageObjects = 0;
        int numNullClasses = 0;
        int numSpacelessObjects = 0;
        int numResizedObjects = 0;

        int queueSize = 0;

        Locks::mutator_lock_->ReaderLock(self);
        while (!done) {
            mirror::Object * object = nullptr;

            writeQueueMutex.ExclusiveLock(self);
            queueSize = writeQueue.size();
            if (queueSize == 0) {
                done = true;
            }
            else {
                object = writeQueue.front();
                writeQueue.erase(writeQueue.begin());
                writeSet.erase(object);
            }
            writeQueueMutex.ExclusiveUnlock(self);

            bool objectInSpace = false;
            gc::Heap * heap = Runtime::Current()->GetHeap();
            for (auto & space : heap->GetContinuousSpaces()) {
                if (space->Contains(object)) {
                    objectInSpace = true;
                }
            }
            for (auto & space : heap->GetDiscontinuousSpaces()) {
                if (space->Contains(object)) {
                    objectInSpace = true;
                }
            }
            if (!objectInSpace) {
                numSpacelessObjects++;
            }

            bool validObject = false;
            if (object != nullptr && objectInSpace) {
                object->SetIgnoreReadFlag();
                if (object->GetPadding() == MAGIC_NUM) {
                    if (object->GetClass() != nullptr) {
                        validObject = true;
                    }
                    else {
                        numNullClasses++;
                    }
                }
                else {
                    numGarbageObjects++;
                }
                object->ClearIgnoreReadFlag();
            }

            if (validObject) {
                object->SetIgnoreReadFlag();
                size_t objectSize = object->SizeOf();
                object->ClearIgnoreReadFlag();

                char * objectData = new char[objectSize];
                LockObjects();
                object->ClearDirtyBit();
                std::memcpy(objectData, object, objectSize);
                UnlockObjects();

                if (objectOffsetMap.find(object) == objectOffsetMap.end()) {
                    std::streampos offset = swapfile.tellp();
                    swapfile.write(objectData, objectSize);
                    bool writeError = checkStreamError(swapfile,
                            "writing object to swapfile for the first time in WriteTask");
                    if (writeError) {
                        ioError = true;
                    }
                    objectOffsetMap[object] = offset;
                    objectSizeMap[object] = objectSize;

                    swapfileObjects++;
                    swapfileSize += objectSize;
                }
                else {
                    std::streampos offset = objectOffsetMap[object];
                    size_t size = objectSizeMap[object];
                    if (size != objectSize) {
                        numResizedObjects++;
                    }
                    else {
                        std::streampos curpos = swapfile.tellp();
                        swapfile.seekp(offset);
                        swapfile.write(objectData, objectSize);
                        bool writeError = checkStreamError(swapfile,
                                "writing object to swapfile again in WriteTask");
                        if (writeError) {
                            ioError = true;
                        }
                        swapfile.seekp(curpos);
                    }
                }

                delete[] objectData;

                if (ioError) {
                    done = true;
                }

                uint64_t currentTime = NanoTime();
                if (currentTime - startTime > 300000000) {
                    done = true;
                }
            }
        }
        Locks::mutator_lock_->ReaderUnlock(self);

        LOG(INFO) << "NIEL done writing objects in WriteTask; " << queueSize
                  << " objects still in queue; swap file has " << swapfileObjects
                  << " objects, size " << swapfileSize;
        if (numGarbageObjects > 0 || numNullClasses > 0 || numSpacelessObjects > 0
                || numResizedObjects > 0) {
            LOG(INFO) << "NIEL WriteTask irregularities: " << numGarbageObjects
                      << " garbage objects, " << numNullClasses << " null classes, "
                      << numSpacelessObjects << " objects not in any space, "
                      << numResizedObjects << " resized objects";
        }

        if ((double)freedSize / swapfileSize > COMPACT_THRESHOLD) {
            CompactSwapFile();
            validateSwapFile();
        }

        uint64_t nanoTime = NanoTime();
        uint64_t targetTime = nanoTime + 3000000000;
        Runtime * runtime = Runtime::Current();
        gc::Heap * curHeap = (runtime == nullptr ? nullptr : runtime->GetHeap());
        gc::TaskProcessor * taskProcessor =
            (curHeap == nullptr ? nullptr : curHeap->GetTaskProcessor());
        if (taskProcessor != nullptr && taskProcessor->IsRunning()) {
            if (ioError) {
                LOG(INFO) << "NIEL not scheduling WriteTask again due to IO error";
            }
            else {
                taskProcessor->AddTask(self, new WriteTask(targetTime));
            }
        }
    }
};

void GcRecordFree(Thread * self, mirror::Object * object) {
    writeQueueMutex.ExclusiveLock(self);
    auto writeQueuePos = std::find(writeQueue.begin(), writeQueue.end(), object);
    if (writeQueuePos != writeQueue.end()) {
        writeQueue.erase(writeQueuePos);
        writeSet.erase(object);
    }
    writeQueueMutex.ExclusiveUnlock(self);

    if (objectOffsetMap.count(object) && objectSizeMap.count(object)) {
        freedObjects++;
        freedSize += objectSizeMap[object];
        objectOffsetMap.erase(object);
        objectSizeMap.erase(object);
    }
    else if (objectOffsetMap.count(object) || objectSizeMap.count(object)) {
        LOG(INFO) << "NIEL ERROR: object in one of the object maps but not the other";
    }
}

void InitIfNecessary() {
    uint32_t curPid = getpid();
    if (curPid != pid) {
        pid = curPid;
        std::string swapfilePath("/data/data/" + getPackageName() + "/swapfile");
        openFile(swapfilePath, swapfile);
        objectOffsetMap.clear();
        objectSizeMap.clear();
        writeQueue.clear();
        writeSet.clear();

        swapfile.write((char *)&pid, 4);
        swapfile.flush();
        bool ioError = checkStreamError(swapfile, "after opening swapfile");

        gc::Heap * heap = Runtime::Current()->GetHeap();
        if (heap->GetTaskProcessor() != nullptr && heap->GetTaskProcessor()->IsRunning()) {
            if (ioError) {
                LOG(INFO) << "NIEL not scheduling first WriteTask due to IO error";
            }
            else {
                heap->GetTaskProcessor()->AddTask(Thread::Current(), new WriteTask(NanoTime()));
            }
        }
    }
}

void CompactSwapFile() {
    LOG(INFO) << "NIEL compacting swap file";

    std::string swapfilePath("/data/data/" + getPackageName() + "/swapfile");
    std::string oldSwapfilePath("/data/data/" + getPackageName() + "/oldswapfile");
    bool ioError = false;

    freedObjects = 0;
    freedSize = 0;
    swapfileObjects = 0;
    swapfileSize = 0;

    swapfile.close();
    rename(swapfilePath.c_str(), oldSwapfilePath.c_str());

    std::fstream oldSwapfile;
    oldSwapfile.open(oldSwapfilePath, std::ios::binary | std::ios::in);

    openFile(swapfilePath, swapfile);

    std::map<void *, std::streampos> newObjectOffsetMap;

    if (checkStreamError(oldSwapfile, "in oldSwapfile before compaction")) {
        ioError = true;
    }
    if (checkStreamError(swapfile, "in swapfile before compaction")) {
        ioError = true;
    }

    for (auto it = objectOffsetMap.begin(); it != objectOffsetMap.end(); it++) {
        if (ioError == true) {
            break;
        }

        void * object = it->first;
        std::streampos oldPos = it->second;
        size_t objectSize = objectSizeMap[object];
        char * objectData = new char[objectSize];

        oldSwapfile.seekg(oldPos);
        oldSwapfile.read(objectData, objectSize);

        std::streampos newPos = swapfile.tellp();
        swapfile.write(objectData, objectSize);
        newObjectOffsetMap[object] = newPos;
        delete[] objectData;

        swapfileObjects++;
        swapfileSize += objectSize;

        if (checkStreamError(oldSwapfile, "in oldSwapfile during compaction")) {
            ioError = true;
        }
        if (checkStreamError(swapfile, "in swapfile during compaction")) {
            ioError = true;
        }
    }

    objectOffsetMap = newObjectOffsetMap;

    oldSwapfile.close();

    if (ioError) {
        LOG(INFO) << "NIEL detected errors while compacting swap file";
    }
    else {
        LOG(INFO) << "NIEL finished compacting swap file; new swap file has " << swapfileObjects
                  << " objects, size " << swapfileSize;
    }
}

void CheckAndUpdate(mirror::Object * object) SHARED_REQUIRES(Locks::mutator_lock_) {
    object->SetIgnoreReadFlag();
    size_t objectSize = object->SizeOf();
    object->ClearIgnoreReadFlag();

    uint32_t readCounterVal = object->GetReadCounter();
    uint32_t writeCounterVal = object->GetWriteCounter();

    bool wasRead = (readCounterVal > 0);
    bool wasWritten = (writeCounterVal > 0);

    //uint32_t rsrVal = object->GetReadShiftRegister();
    //uint32_t wsrVal = object->GetWriteShiftRegister();

    if (objectSize > 5000) {
        Thread * self = Thread::Current();
        writeQueueMutex.ExclusiveLock(self);
        if (!writeSet.count(object)) {
            writeSet.insert(object);
            writeQueue.push_back(object);
            object->SetPadding(MAGIC_NUM);
        }
        writeQueueMutex.ExclusiveUnlock(self);
    }

    if (wasRead) {
        object->ClearReadCounter();
    }
    if (wasWritten) {
        object->ClearWriteCounter();
    }

    object->UpdateReadShiftRegister(wasRead);
    object->UpdateWriteShiftRegister(wasWritten);
}

void LockObjects() {
    pthread_mutex_lock(&objectLock);
}

void UnlockObjects() {
    pthread_mutex_unlock(&objectLock);
}

void validateSwapFile() {
    bool error = false;

    LOG(INFO) << "NIEL starting swap file validation";

    if (objectOffsetMap.size() != objectSizeMap.size()) {
        LOG(INFO) << "NIEL ERROR: objectOffsetMap and objectSizeMap sizes differ";
        error = true;
    }

    std::streampos curPos = swapfile.tellp();
    if (checkStreamError(swapfile, "validation initial tellp()")) {
        error = true;
    }

    for (auto it = objectOffsetMap.begin(); it != objectOffsetMap.end(); it++) {
        void * object = it->first;
        std::streampos offset = it->second;

        if (!objectSizeMap.count(object)) {
            LOG(INFO) << "NIEL ERROR: objectSizeMap does not contain object " << object;
            error = true;
        }

        size_t objectSize = objectSizeMap[object];
        char * objectData = new char[objectSize];

        swapfile.seekg(offset);
        if (checkStreamError(swapfile, "validation seekg()")) {
            error = true;
        }
        swapfile.read(objectData, objectSize);
        if (checkStreamError(swapfile, "validation read()")) {
            error = true;
        }

        if (objectSize < 16) {
            LOG(INFO) << "NIEL ERROR: object size " << objectSize << " is too small";
            error = true;
        }

        int * padding = (int *)&objectData[12];
        if (*padding != MAGIC_NUM) {
            LOG(INFO) << "NIEL ERROR: object padding does not match magic num";
            error = true;
        }
    }

    swapfile.seekp(curPos);
    if (checkStreamError(swapfile, "validation final seekp()")) {
        error = true;
    }

    if (error) {
        LOG(INFO) << "NIEL swap file validation detected errors";
    }
    else {
        LOG(INFO) << "NIEL swap file validation successful";
    }
}

std::string getPackageName() {
    std::ifstream cmdlineFile("/proc/self/cmdline");
    std::string cmdline;
    getline(cmdlineFile, cmdline);
    cmdlineFile.close();
    return cmdline.substr(0, cmdline.find((char)0));
}

void openFile(const std::string & path, std::fstream & stream) {
    stream.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!stream) {
        stream.close();
        stream.open(path, std::ios::binary | std::ios::out);
        stream.close();
        stream.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    }
}

bool checkStreamError(const std::ios & stream, const std::string & msg) {
    bool error = !stream;
    if (error) {
        LOG(ERROR) << "NIEL stream error: " << msg << " (" << stream.good() << " "
                   << stream.eof() << " " << stream.fail() << " " << stream.bad() << ")";
    }
    return error;
}

} // namespace swap
} // namespace niel
} // namespace art
