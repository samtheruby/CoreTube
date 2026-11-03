// SPDX-License-Identifier: BSD-3-Clause

#include <android/hidl/memory/1.0/IMemory.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace android::hardware {
using android::hidl::memory::V1_0::IMemory;

class FileMemory final : public IMemory {
public:
  FileMemory(void *pointer, size_t size) : pointer_(pointer), size_(size) {}
  ~FileMemory() override { munmap(pointer_, size_); }
  Return<void> update() override { return Void(); }
  Return<void> read() override { return Void(); }
  Return<void> commit() override { return Void(); }
  Return<void> updateRange(uint64_t start, uint64_t length) override {
    if (start > size_ || length > size_ - start)
      return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
    return Void();
  }
  Return<void> readRange(uint64_t start, uint64_t length) override {
    return updateRange(start, length);
  }
  Return<void *> getPointer() override { return pointer_; }
  Return<uint64_t> getSize() override { return size_; }

private:
  void *pointer_;
  size_t size_;
};

// Same-process HIDL buffers need no Binder service or Android ashmem driver.
sp<IMemory> mapMemory(const hidl_memory &memory) {
  const native_handle_t *handle = memory.handle();
  struct stat info;
  if (memory.name() != "coreelec-drm" || !handle || handle->numFds != 1 ||
      handle->numInts != 0 || !memory.size() ||
      memory.size() > 32 * 1024 * 1024 || fstat(handle->data[0], &info) ||
      !S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<uint64_t>(info.st_size) < memory.size())
    return nullptr;
  void *pointer = mmap(nullptr, memory.size(), PROT_READ | PROT_WRITE,
                       MAP_SHARED, handle->data[0], 0);
  if (pointer == MAP_FAILED)
    return nullptr;
  return new FileMemory(pointer, memory.size());
}
} // namespace android::hardware
