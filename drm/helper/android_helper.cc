// SPDX-License-Identifier: BSD-3-Clause
// Built against Android 9's headers and the device's unmodified Bionic runtime.
#include "android_protocol.h"
#include <android/hardware/drm/1.0/ICryptoFactory.h>
#include <android/hardware/drm/1.0/IDrmPluginListener.h>
#include <android/hardware/drm/1.1/IDrmFactory.h>
#include <android/hardware/drm/1.1/IDrmPlugin.h>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <vector>

namespace drm0 = android::hardware::drm::V1_0;
namespace drm1 = android::hardware::drm::V1_1;
using android::sp;
using android::hardware::hidl_array;
using android::hardware::hidl_string;
using android::hardware::hidl_vec;
using android::hardware::Return;
using android::hardware::Void;
using drm0::Status;
using namespace android_drm;

static std::string Bytes(const hidl_vec<uint8_t> &value) {
  return std::string(reinterpret_cast<const char *>(value.data()),
                     value.size());
}
static hidl_vec<uint8_t> Vector(const std::string &value) {
  return hidl_vec<uint8_t>(std::vector<uint8_t>(value.begin(), value.end()));
}
static std::string Id(const hidl_vec<uint8_t> &value) {
  static const char hex[] = "0123456789abcdef";
  std::string id;
  for (uint8_t byte : value) {
    id += hex[byte >> 4];
    id += hex[byte & 15];
  }
  return id;
}

class Listener final : public drm0::IDrmPluginListener {
public:
  Return<void> sendEvent(drm0::EventType type, const hidl_vec<uint8_t> &session,
                         const hidl_vec<uint8_t> &) override {
    std::lock_guard<std::mutex> lock(mutex);
    if (type == drm0::EventType::KEY_NEEDED)
      renewals.push_back(Id(session));
    if (type == drm0::EventType::SESSION_RECLAIMED) {
      Packet event;
      event.Put(Closed);
      event.Put(Id(session));
      events.push_back(event);
    }
    return Void();
  }
  Return<void> sendExpirationUpdate(const hidl_vec<uint8_t> &,
                                    int64_t) override {
    return Void();
  }
  Return<void> sendKeysChange(const hidl_vec<uint8_t> &session,
                              const hidl_vec<drm0::KeyStatus> &keys,
                              bool) override {
    Packet event;
    event.Put(Keys);
    event.Put(Id(session));
    event.Put(keys.size());
    for (const auto &key : keys) {
      event.Put(Bytes(key.keyId));
      event.Put(uint32_t(key.type));
    }
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back(event);
    return Void();
  }
  std::mutex mutex;
  std::vector<Packet> events;
  std::vector<std::string> renewals;
};

struct Session {
  hidl_vec<uint8_t> raw;
  sp<drm0::ICryptoPlugin> crypto;
};

class Helper {
public:
  bool Initialize(int memory_fd, int provision_fd) {
    provision_fd_ = provision_fd;
    void *library = dlopen("/vendor/lib/libwvhidl.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      fprintf(stderr, "android-drm: %s\n", dlerror());
      return false;
    }
    auto drm_entry = reinterpret_cast<drm1::IDrmFactory *(*)()>(
        dlsym(library, "createDrmFactory"));
    auto crypto_entry = reinterpret_cast<drm0::ICryptoFactory *(*)()>(
        dlsym(library, "createCryptoFactory"));
    if (!drm_entry || !crypto_entry)
      return false;
    sp<drm1::IDrmFactory> factory = drm_entry();
    factory_ = crypto_entry();
    if (!factory || !factory_)
      return false;
    sp<drm0::IDrmPlugin> base;
    Status status = Status::ERROR_DRM_UNKNOWN;
    auto created = factory->createPlugin(
        uuid_, "org.coreelec.tizentube",
        [&](Status result, const sp<drm0::IDrmPlugin> &plugin) {
          status = result;
          base = plugin;
        });
    if (!created.isOk() || status != Status::OK || !base)
      return false;
    auto selected = base->setPropertyString("securityLevel", "L3");
    if (!selected.isOk() || Status(selected) != Status::OK)
      return false;
    auto cast = drm1::IDrmPlugin::castFrom(base);
    if (!cast.isOk())
      return false;
    plugin_ = cast;
    if (!plugin_)
      return false;
    listener_ = new Listener;
    if (!plugin_->setListener(listener_).isOk())
      return false;
    struct stat info;
    if (fstat(memory_fd, &info) || !S_ISREG(info.st_mode) ||
        info.st_size != 2 * kCapacity)
      return false;
    handle_ = native_handle_create(1, 0);
    if (!handle_)
      return false;
    handle_->data[0] = memory_fd;
    memory_ =
        android::hardware::hidl_memory("coreelec-drm", handle_, 2 * kCapacity);
    return true;
  }

  Packet Handle(Packet &request) {
    uint32_t command = request.Get();
    Packet result;
    if (command == Hello) {
      uint32_t version = request.Get();
      result.Put(request.Done() && version == kVersion ? Ok : Failed);
    } else if (command == Open) {
      std::string type = request.String(), init = request.String();
      result = request.Done() ? OpenSession(type, init) : Failure();
    } else if (command == Certificate) {
      std::string cert = request.String();
      if (!request.Done())
        return Failure();
      auto status =
          plugin_->setPropertyByteArray("serviceCertificate", Vector(cert));
      result.Put(status.isOk() && Status(status) == Status::OK ? Ok : Failed);
    } else if (command == Poll && request.Done()) {
      result.Put(Ok);
    } else {
      std::string id = request.String();
      auto found = sessions_.find(id);
      if (found == sessions_.end())
        return Failure();
      auto &session = found->second;
      if (command == Update) {
        std::string response = request.String();
        if (!request.Done())
          return Failure();
        Status status = Status::ERROR_DRM_UNKNOWN;
        auto supplied = plugin_->provideKeyResponse(
            session.raw, Vector(response),
            [&](Status value, const hidl_vec<uint8_t> &) { status = value; });
        result.Put(supplied.isOk() && status == Status::OK ? Ok : Failed);
      } else if (command == Close && request.Done()) {
        auto status = plugin_->closeSession(session.raw);
        sessions_.erase(found);
        result.Put(status.isOk() && Status(status) == Status::OK ? Ok : Failed);
      } else if (command == Decrypt) {
        result = DecryptSample(request, session);
      } else
        return Failure();
    }
    Drain(&result);
    return result;
  }

private:
  static Packet Failure() {
    Packet result;
    result.Put(Failed);
    result.Put(0);
    return result;
  }
  bool ProvisionDevice() {
    Packet request;
    request.Put(Provision);
    Status status = Status::ERROR_DRM_UNKNOWN;
    auto generated = plugin_->getProvisionRequest(
        "none", "",
        [&](Status value, const hidl_vec<uint8_t> &data,
            const hidl_string &url) {
          status = value;
          request.Put(std::string(url.c_str()));
          request.Put(Bytes(data));
        });
    Packet response;
    if (!generated.isOk() || status != Status::OK ||
        !Send(provision_fd_, request) || !Receive(provision_fd_, &response) ||
        response.Get() != Ok)
      return false;
    std::string body = response.String();
    if (!response.Done())
      return false;
    auto supplied = plugin_->provideProvisionResponse(
        Vector(body), [&](Status value, const hidl_vec<uint8_t> &,
                          const hidl_vec<uint8_t> &) { status = value; });
    fprintf(stderr, "android-drm: provisioning status %u\n", unsigned(status));
    return supplied.isOk() && status == Status::OK;
  }
  bool KeyRequest(const std::string &id, const std::string &mime,
                  const std::string &init) {
    auto found = sessions_.find(id);
    if (found == sessions_.end())
      return false;
    Status status = Status::ERROR_DRM_UNKNOWN;
    auto requested = plugin_->getKeyRequest_1_1(
        found->second.raw, Vector(init), mime, drm0::KeyType::STREAMING, {},
        [&](Status value, const hidl_vec<uint8_t> &bytes,
            drm1::KeyRequestType type, const hidl_string &) {
          status = value;
          if (value != Status::OK || bytes.size() == 0)
            return;
          Packet event;
          event.Put(Message);
          event.Put(id);
          event.Put(type == drm1::KeyRequestType::RENEWAL   ? 1
                    : type == drm1::KeyRequestType::RELEASE ? 2
                                                            : 0);
          event.Put(Bytes(bytes));
          std::lock_guard<std::mutex> lock(listener_->mutex);
          listener_->events.push_back(event);
        });
    return requested.isOk() && status == Status::OK;
  }
  Packet OpenSession(const std::string &type, const std::string &init) {
    if (sessions_.size() >= 32 || (type != "cenc" && type != "webm"))
      return Failure();
    Session session;
    Status status = Status::ERROR_DRM_UNKNOWN;
    auto open = [&] {
      auto call = plugin_->openSession_1_1(
          drm1::SecurityLevel::SW_SECURE_CRYPTO,
          [&](Status value, const hidl_vec<uint8_t> &raw) {
            status = value;
            session.raw = raw;
          });
      return call.isOk();
    };
    bool opened = open();
    if (opened && status == Status::ERROR_DRM_NOT_PROVISIONED &&
        ProvisionDevice())
      opened = open();
    if (!opened || status != Status::OK || session.raw.size() == 0)
      return Failure();
    std::string id = Id(session.raw);
    bool level_ok = false;
    auto level = plugin_->getSecurityLevel(
        session.raw, [&](Status value, drm1::SecurityLevel security) {
          level_ok = value == Status::OK &&
                     security == drm1::SecurityLevel::SW_SECURE_CRYPTO;
        });
    auto created = factory_->createPlugin(
        uuid_, session.raw,
        [&](Status value, const sp<drm0::ICryptoPlugin> &crypto) {
          status = value;
          session.crypto = crypto;
        });
    bool ok = level.isOk() && level_ok && created.isOk() &&
              status == Status::OK && session.crypto;
    if (ok) {
      auto mapped = session.crypto->setSharedBufferBase(memory_, 0);
      ok = mapped.isOk();
    }
    if (ok) {
      sessions_[id] = session;
      ok = KeyRequest(id, type == "webm" ? "video/webm" : "video/mp4", init);
    }
    if (!ok) {
      sessions_.erase(id);
      (void)plugin_->closeSession(session.raw).isOk();
      return Failure();
    }
    fprintf(stderr, "android-drm: opened L3 session\n");
    Packet result;
    result.Put(Ok);
    result.Put(id);
    return result;
  }
  Packet DecryptSample(Packet &request, const Session &session) {
    std::string key = request.String(), iv = request.String();
    uint32_t scheme = request.Get(), crypt = request.Get(),
             skip = request.Get();
    uint32_t size = request.Get(), count = request.Get();
    if (key.size() != 16 || iv.size() != 16 || !size || size > kCapacity ||
        !count || count > 65536 || (scheme != 1 && scheme != 2))
      return Failure();
    hidl_vec<drm0::SubSample> samples;
    samples.resize(count);
    uint64_t total = 0;
    for (auto &sample : samples) {
      sample.numBytesOfClearData = request.Get();
      sample.numBytesOfEncryptedData = request.Get();
      total +=
          uint64_t(sample.numBytesOfClearData) + sample.numBytesOfEncryptedData;
    }
    if (!request.Done() || total != size)
      return Failure();
    drm0::DestinationBuffer destination = {};
    destination.type = drm0::BufferType::SHARED_MEMORY;
    destination.nonsecureMemory = {0, kCapacity, size};
    Status status = Status::ERROR_DRM_UNKNOWN;
    uint32_t written = 0;
    auto decrypted = session.crypto->decrypt(
        false,
        hidl_array<uint8_t, 16>(reinterpret_cast<const uint8_t *>(key.data())),
        hidl_array<uint8_t, 16>(reinterpret_cast<const uint8_t *>(iv.data())),
        scheme == 1 ? drm0::Mode::AES_CTR : drm0::Mode::AES_CBC, {crypt, skip},
        samples, {0, 0, size}, 0, destination,
        [&](Status value, uint32_t bytes, const hidl_string &) {
          status = value;
          written = bytes;
        });
    Packet result;
    result.Put(decrypted.isOk() && status == Status::OK && written == size ? Ok
               : status == Status::ERROR_DRM_NO_LICENSE ? NoKey
                                                        : Failed);
    return result;
  }
  void Drain(Packet *result) {
    std::vector<std::string> renewals;
    {
      std::lock_guard<std::mutex> lock(listener_->mutex);
      renewals.swap(listener_->renewals);
    }
    for (const auto &id : renewals)
      KeyRequest(id, "", "");
    std::vector<Packet> events;
    {
      std::lock_guard<std::mutex> lock(listener_->mutex);
      events.swap(listener_->events);
    }
    result->Put(events.size());
    for (const auto &event : events)
      result->data += event.data;
  }
  const uint8_t uuid_bytes_[16] = {0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6,
                                   0x4a, 0xce, 0xa3, 0xc8, 0x27, 0xdc,
                                   0xd5, 0x1d, 0x21, 0xed};
  const hidl_array<uint8_t, 16> uuid_{uuid_bytes_};
  sp<drm1::IDrmPlugin> plugin_;
  sp<drm0::ICryptoFactory> factory_;
  sp<Listener> listener_;
  std::map<std::string, Session> sessions_;
  native_handle_t *handle_ = nullptr;
  android::hardware::hidl_memory memory_;
  int provision_fd_ = -1;
};

int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  umask(0077);
  int control = atoi(argv[1]), memory = atoi(argv[2]),
      provision = atoi(argv[3]);
  Helper helper;
  if (!helper.Initialize(memory, provision)) {
    fputs("android-drm: initialization failed\n", stderr);
    return 3;
  }
  Packet request;
  while (Receive(control, &request)) {
    Packet response = helper.Handle(request);
    if (!Send(control, response))
      break;
  }
  // Vendor workers retain references until process exit.
  _exit(0);
}
