#include "niel_swap.h"

#include "gc/heap.h"
#include "gc/task_processor.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "runtime.h"

#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <vector>

namespace art {

namespace niel {

namespace swap {

/* Constants */
const int MAGIC_NUM = 42424242;

/* Internal functions */
std::string getPackageName();
void openFile(const std::string & path, std::fstream & stream);
bool checkStreamError(const std::ios & stream, const std::string & msg);

/* Variables */
std::fstream swapfile;
uint32_t pid = 0;
std::map<void *, std::streampos> objectOffsetMap;
std::map<void *, size_t> objectSizeMap;
std::vector<mirror::Object *> writeQueue;
Mutex writeQueueMutex("WriteQueueMutex", kLoggingLock);

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

        Locks::mutator_lock_->ReaderLock(self);
        while (!done) {
            mirror::Object * object = nullptr;

            writeQueueMutex.ExclusiveLock(self);
            if (writeQueue.size() == 0) {
                done = true;
            }
            else {
                object = writeQueue.front();
                writeQueue.erase(writeQueue.begin());
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
                object->SetIgnoreAccessFlag();
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
                object->ClearIgnoreAccessFlag();
            }

            if (validObject) {
                object->SetIgnoreAccessFlag();
                size_t objectSize = object->SizeOf();
                object->ClearIgnoreAccessFlag();
                if (objectOffsetMap.find(object) == objectOffsetMap.end()) {
                    std::streampos offset = swapfile.tellp();
                    swapfile.write((char *)object, objectSize);
                    bool writeError = checkStreamError(swapfile,
                            "writing object to swapfile for the first time in WriteTask");
                    if (writeError) {
                        ioError = true;
                    }
                    objectOffsetMap[object] = offset;
                    objectSizeMap[object] = objectSize;
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
                        swapfile.write((char *)object, objectSize);
                        bool writeError = checkStreamError(swapfile,
                                "writing object to swapfile again in WriteTask");
                        if (writeError) {
                            ioError = true;
                        }
                        swapfile.seekp(curpos);
                    }
                }

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

        if (numGarbageObjects > 0 || numNullClasses > 0 || numSpacelessObjects > 0
                || numResizedObjects > 0) {
            LOG(INFO) << "NIEL WriteTask irregularities: " << numGarbageObjects
                      << " garbage objects, " << numNullClasses << " null classes, "
                      << numSpacelessObjects << " objects not in any space, "
                      << numResizedObjects << " resized objects";
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
    }

    writeQueueMutex.ExclusiveUnlock(self);
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

void UpdateAndCheck(mirror::Object * object) SHARED_REQUIRES(Locks::mutator_lock_) {
    object->SetIgnoreAccessFlag();
    size_t objectSize = object->SizeOf();
    object->ClearIgnoreAccessFlag();

    uint32_t readCounterVal = object->GetReadCounter();
    uint32_t writeCounterVal = object->GetWriteCounter();

    bool wasRead = false;
    bool wasWritten = false;

    if (readCounterVal > 0) {
        object->ClearReadCounter();
        wasRead = true;
    }

    if (writeCounterVal > 0) {
        object->ClearWriteCounter();
        wasWritten = true;
    }

    object->UpdateReadShiftRegister(wasRead);
    object->UpdateWriteShiftRegister(wasWritten);

    //uint32_t rsrVal = object->GetReadShiftRegister();
    //uint32_t wsrVal = object->GetWriteShiftRegister();

    if (objectSize > 1000) {
        Thread * self = Thread::Current();
        writeQueueMutex.ExclusiveLock(self);
        writeQueue.push_back(object);
        object->SetPadding(MAGIC_NUM);
        writeQueueMutex.ExclusiveUnlock(self);
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
