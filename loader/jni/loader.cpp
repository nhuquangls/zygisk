#include <android/dlext.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <jni.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>

#include "zygisk.hpp"
#include "specialize_socket.hpp"
extern "C" void cf_input_companion(int fd);

namespace {

constexpr char kTargetProcess[] = "com.vnggames.cfl.crossfirelegends";
constexpr char kPayloadPath[] = "payload/libgcloudsync.so";
// Keep the existing app-local loading path for compatibility with this app.
constexpr char kDataFilesDir[] =
    "/data/user/0/com.vnggames.cfl.crossfirelegends/files";
constexpr char kOutPayloadName[] = "libgcloudsync.so";

// Detach a loaded image from its file identity, the way Riru and
// AndKittyInjector's --hide do it, but in-process: every segment is remapped
// onto anonymous memory at the identical address (so all code, data and
// registered pointers stay valid) and the ELF header at the image base is
// overwritten with pseudorandom bytes. Afterwards /proc/self/maps shows only
// anonymous regions for the image and memory scans find no ELF header.
// Region protections, including the linker's RELRO split, are rebuilt from
// the program headers, so the layout stays byte-for-byte equivalent.
bool anonymize_mapped_image(void *symbol, size_t *hidden_bytes) {
    struct Region {
        uintptr_t start;
        size_t size;
        int prot;
    };

    Dl_info info{};
    if (hidden_bytes != nullptr) *hidden_bytes = 0;
    if (symbol == nullptr || dladdr(symbol, &info) == 0) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    if (base == 0) return false;
    const auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(base);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return false;
    }
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    if (page == 0) return false;
    const uintptr_t mask = page - 1;
    auto page_floor = [mask](uintptr_t value) { return value & ~mask; };
    auto page_ceil = [mask](uintptr_t value) { return (value + mask) & ~mask; };

    Region regions[12];
    size_t region_count = 0;
    uintptr_t relro_start = 0, relro_end = 0;
    const auto *phdrs = reinterpret_cast<const Elf64_Phdr *>(base + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        const Elf64_Phdr &phdr = phdrs[i];
        if (phdr.p_type == PT_LOAD) {
            const uintptr_t start = base + page_floor(phdr.p_vaddr);
            const uintptr_t end = base + page_ceil(phdr.p_vaddr + phdr.p_memsz);
            if (end <= start || region_count >= sizeof(regions) / sizeof(regions[0])) {
                return false;
            }
            int prot = 0;
            if (phdr.p_flags & PF_R) prot |= PROT_READ;
            if (phdr.p_flags & PF_W) prot |= PROT_WRITE;
            if (phdr.p_flags & PF_X) prot |= PROT_EXEC;
            regions[region_count].start = start;
            regions[region_count].size = end - start;
            regions[region_count].prot = prot;
            ++region_count;
        } else if (phdr.p_type == PT_GNU_RELRO) {
            // The linker makes exactly this range read-only after relocation.
            relro_start = base + page_floor(phdr.p_vaddr);
            relro_end = base + page_ceil(phdr.p_vaddr + phdr.p_memsz);
        }
    }
    if (region_count == 0) return false;
    if (relro_end > relro_start) {
        for (size_t i = 0; i < region_count; ++i) {
            Region &region = regions[i];
            if (region.start >= relro_end || region.start + region.size <= relro_start) {
                continue;
            }
            // Split the region so the RELRO pages become read-only again
            // after the anonymous remap, matching the linker's layout.
            const uintptr_t overlap_start = region.start < relro_start ? relro_start : region.start;
            const uintptr_t overlap_end = region.start + region.size < relro_end
                                              ? region.start + region.size
                                              : relro_end;
            const size_t room = sizeof(regions) / sizeof(regions[0]) - region_count;
            const size_t needed = (overlap_start > region.start ? 1u : 0u) +
                                  (overlap_end < region.start + region.size ? 1u : 0u);
            if (needed > room) return false;
            if (overlap_end < region.start + region.size) {
                regions[region_count] = {overlap_end, region.start + region.size - overlap_end, region.prot};
                ++region_count;
            }
            if (overlap_start > region.start) {
                regions[region_count] = {region.start, overlap_start - region.start, region.prot};
                ++region_count;
            }
            region.start = overlap_start;
            region.size = overlap_end - overlap_start;
            region.prot = PROT_READ;
        }
    }

    for (size_t i = 0; i < region_count; ++i) {
        const Region &region = regions[i];
        if (region.size == 0) continue;
        unsigned char *backup = static_cast<unsigned char *>(malloc(region.size));
        if (backup == nullptr) return false;
        memcpy(backup, reinterpret_cast<const void *>(region.start), region.size);
        if (region.start == base && region.size >= sizeof(Elf64_Ehdr)) {
            // First region: replace the ELF header with pseudorandom bytes.
            // The program headers behind it stay intact for dl_iterate_phdr.
            uint64_t state = base ^ 0x9e3779b97f4a7c15ULL;
            for (size_t b = 0; b < sizeof(Elf64_Ehdr); ++b) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                backup[b] = static_cast<unsigned char>(state >> 33);
            }
        }
        if (munmap(reinterpret_cast<void *>(region.start), region.size) != 0) {
            free(backup);
            return false;
        }
        void *fresh = mmap(reinterpret_cast<void *>(region.start), region.size,
                           PROT_READ | PROT_WRITE,
                           MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (fresh != reinterpret_cast<void *>(region.start)) {
            free(backup);
            return false;
        }
        memcpy(reinterpret_cast<void *>(region.start), backup, region.size);
        free(backup);
        if (mprotect(reinterpret_cast<void *>(region.start), region.size, region.prot) != 0) {
            return false;
        }
        if (region.prot & PROT_EXEC) {
            __builtin___clear_cache(reinterpret_cast<char *>(region.start),
                                    reinterpret_cast<char *>(region.start + region.size));
        }
        if (hidden_bytes != nullptr) *hidden_bytes += region.size;
    }
    return true;
}

class CfReadOnlyModule final : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (args == nullptr || args->nice_name == nullptr) {
            unloadForNonTarget();
            return;
        }

        const char *process_name = env_->GetStringUTFChars(args->nice_name, nullptr);
        target_process_ = process_name != nullptr &&
                          strcmp(process_name, kTargetProcess) == 0;
        if (process_name != nullptr) {
            env_->ReleaseStringUTFChars(args->nice_name, process_name);
        }

        if (!target_process_) {
            unloadForNonTarget();
            return;
        }

        const int module_dir = api_->getModuleDir();
        if (module_dir < 0) {
            return;
        }

        const int payload_fd = openat(module_dir, kPayloadPath, O_RDONLY | O_CLOEXEC);
        close(module_dir);
        if (payload_fd < 0) {
            return;
        }

        // Read while the module directory is still accessible. No payload FD
        // needs to survive specialization; loading still uses Android's linker.
        struct stat st {};
        if (fstat(payload_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) {
            close(payload_fd);
            return;
        }
        char *bytes = static_cast<char *>(malloc(static_cast<size_t>(st.st_size)));
        if (!bytes) {
            close(payload_fd);
            return;
        }
        size_t total = 0;
        const size_t size = static_cast<size_t>(st.st_size);
        while (total < size) {
            const ssize_t n = read(payload_fd, bytes + total, size - total);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        close(payload_fd);
        if (total != size) {
            free(bytes);
            return;
        }
        payload_size_ = size;
        payload_bytes_ = bytes;

        if (!input_socket_.capture(api_->connectCompanion())) {
            return;
        }
        // Some specialization paths reject exemption. Keep the socket pending
        // and check its actual identity in post; never trust the FD number alone.
        (void)api_->exemptFd(input_socket_.get());
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        // The payload is self-contained (own mappings, own imports), so this
        // library is not needed in the target process after this hook returns.
        // Request the unload on every exit path; Zygisk unmaps it afterwards
        // and the module's path disappears from the process maps.
        struct UnloadModuleOnReturn {
            zygisk::Api *api;
            ~UnloadModuleOnReturn() { api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY); }
        } unload{api_};
        // take() disowns a closed/reused FD without touching its replacement.
        int input_fd = input_socket_.take();
        struct CloseInputOnReturn {
            int &fd;
            ~CloseInputOnReturn() { if (fd >= 0) close(fd); }
        } close_input{input_fd};
        struct FreePayloadOnReturn {
            char *bytes;
            ~FreePayloadOnReturn() { free(bytes); }
        } payload{payload_bytes_};
        payload_bytes_ = nullptr;
        const size_t size = payload_size_;
        payload_size_ = 0;
        if (!target_process_ || !payload.bytes || size == 0) {
            return;
        }
        if (mkdir(kDataFilesDir, 0700) != 0 && errno != EEXIST) {
            return;
        }

        char out_path[256];
        snprintf(out_path, sizeof(out_path), "%s/%s", kDataFilesDir,
                 kOutPayloadName);

        int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
        if (out_fd < 0) {
            return;
        }
        size_t written = 0;
        while (written < size) {
            ssize_t n = write(out_fd, payload.bytes + written, size - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                break;
            }
            written += static_cast<size_t>(n);
        }
        close(out_fd);
        free(payload.bytes);
        payload.bytes = nullptr;
        if (written != size) {
            unlink(out_path);
            return;
        }

        // Load the prepared file descriptor through Android's linker API.
        int load_fd = open(out_path, O_RDONLY | O_CLOEXEC);
        if (load_fd < 0) {
            return;
        }

        android_dlextinfo extinfo{};
        extinfo.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
        extinfo.library_fd = load_fd;
        payload_handle_ = android_dlopen_ext(kOutPayloadName, RTLD_NOW | RTLD_LOCAL,
                                             &extinfo);
        close(load_fd);
        if (payload_handle_ == nullptr) {
            unlink(out_path);
            return;
        }
        // The payload is fully mapped through the descriptor, so the on-disk
        // copy is no longer needed. Remove it for the whole session: nothing
        // can stat, scan or hash it afterwards. A later launch rewrites the
        // file before loading, so unlinking here is self-contained.
        unlink(out_path);

        // dlopen does not invoke JNI_OnLoad. Start after loading completes.
        using StartPayload = int (*)(JavaVM *);
        dlerror();
        auto start_payload = reinterpret_cast<StartPayload>(
            dlsym(payload_handle_, "cf_payload_start_vm"));
        if (dlerror() != nullptr || start_payload == nullptr) {
            dlclose(payload_handle_);
            payload_handle_ = nullptr;
            return;
        }
        JavaVM *vm = nullptr;
        if (env_->GetJavaVM(&vm) != JNI_OK || vm == nullptr) {
            return;
        }
        using ConfigureInput = int (*)(int);
        auto configure_input = reinterpret_cast<ConfigureInput>(
            dlsym(payload_handle_, "cf_payload_input_fd"));

        // Resolve every symbol first, then break the image's association
        // with any file: same-address anonymous remap plus a scrambled ELF
        // header. A failure here only costs stealth, never functionality.
        size_t hidden_bytes = 0;
        (void)anonymize_mapped_image(reinterpret_cast<void *>(start_payload),
                                     &hidden_bytes);

        const int input_result = input_fd < 0 ? EBADF :
            (configure_input == nullptr ? ENOSYS : configure_input(input_fd));
        if (input_result == 0) {
            input_fd = -1; // Payload owns the descriptor.
        }
        int start_result = start_payload(vm);
        if (start_result != 0) {
            return;
        }
    }

private:
    void unloadForNonTarget() {
        if (api_ != nullptr) {
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_process_ = false;
    char *payload_bytes_ = nullptr;
    size_t payload_size_ = 0;
    SpecializeSocket input_socket_;
    void *payload_handle_ = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(CfReadOnlyModule)
REGISTER_ZYGISK_COMPANION(cf_input_companion)
