# Knowledge — các thay đổi anti-detection của module (2.2.3 → 2.2.7)

Ghi chú kỹ thuật tích lũy khi làm mờ dấu vết của module Zygisk trong game
`com.vnggames.cfl.crossfirelegends` (Hawk/ACE anti-cheat). Mọi mục đều đã
verify trực tiếp trên device 192.168.5.102 (KernelSU + SUSFS, kernel
6.1.138-android14, ARM64, game UID 10281/u0_a281).

## Timeline các version

| Version | Thay đổi chính | Kết quả verify |
|---|---|---|
| 2.2.3 | Socket cookie/inode validation (nền tảng) | EACCES(13) tồn tại từ version này |
| 2.2.4-diskless | `unlink()` file payload ngay sau dlopen | File biến mất khỏi disk; maps vẫn còn `(deleted)` |
| 2.2.5-scrub | Đổi toàn bộ danh tính + DLCLOSE loader | 9 dòng maps → 3 dòng; 0 tag cũ trong logcat |
| 2.2.6-channelfix | `sepolicy.rule` cho channel socket | EACCES(13) hết; input channel verified lần đầu |
| 2.2.7-anonimage | Anonymous remap + scramble ELF header | **0 dòng** trong maps của game |

---

## 1. v2.2.4 — Unlink payload khỏi disk

`loader/jni/loader.cpp`, trong `postAppSpecialize`, ngay sau khi
`android_dlopen_ext` với `ANDROID_DLEXT_USE_LIBRARY_FD` thành công:

```c
unlink(out_path);   // payload đã map qua fd, file trên disk không còn cần
```

- Payload được buffer **trước** specialization (preAppSpecialize đọc qua
  `getModuleDir()`), ghi ra `/data/user/0/<pkg>/files/libgcloudsync.so` và
  dlopen qua FD sau specialization, rồi unlink.
- Lần launch sau ghi lại file rồi unlink tiếp — tự chứa, không cần dọn.
- Còn sót: mapping vẫn hiện `... (deleted)` trong `/proc/pid/maps` → tell
  kinh điển của injection → xử lý nốt ở 2.2.7.

## 2. v2.2.5 — Đổi toàn bộ danh tính (identity scrub)

### Bảng map cũ → mới

| Hạng mục | Cũ | Mới |
|---|---|---|
| Tên file payload (maps/disk/SONAME) | `libcfreadonly.so` | `libgcloudsync.so` — trùng họ lib thật của game (libgcloud/libgcloudcore) |
| Module id / thư mục | `cf_aim_speed_zygisk` | `rt_shim` |
| Tên tiến trình companion | `zn-zygisk-companion64 cf_aim_speed_zygisk` | `zn-zygisk-companion64 rt_shim` (tên module lộ trong ps!) |
| Log tag loader | `CFZygisk` | `SysLoader` |
| Log tag payload | `CFReadOnly` | `SysRuntime` |
| Log tag Java bridge | `CFAimInput` | `SysInput` |
| Java class | `cf.readonly.AimBridge` | `rt.internal.Bridge` |
| Native methods JNI | `poll/report/running/swipe` | `read/status/active/apply` |
| File zip output | `cf_aim_200px_zygisk.zip` | `rt_shim.zip` |
| Soname DT_SONAME | `libcfreadonly.so` | `libgcloudsync.so` |

Chọn tên `libgcloudsync` sau khi liệt kê 23 lib thật của game
(lib352, libCFStateMachineSystem, libCrashSight, libGCloudVoice, libGPM,
libINTL*, libPluginCrosCurl, libRoosterNN, libTDataMaster, libanogs,
libanort, libcrosCurl, ..., libunity, libxlua) — không trùng, không gây chú ý.

### Log message scrub

Toàn bộ message cũng neutral hóa: "payload" → "image", "companion" →
"control channel", "aim radius=200px..." → "resolver ready", "libil2cpp.so
not found" → "engine library not found", v.v. Binary sau khi strip được scan
bằng ASCII `Contains()` — xác nhận 0 chuỗi `cfreadonly`/`CFZygisk`/`aim`
trong loader và payload.

### DLCLOSE_MODULE_LIBRARY

Sau khi load payload xong, `postAppSpecialize` dùng RAII guard để luôn gọi:

```c
api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
```

→ đường dẫn `/data/adb/modules/rt_shim/zygisk/arm64-v8a.so` **biến khỏi
maps** của game (trước đây 3 dòng). Lưu ý: phải resolve mọi `dlsym` cần
thiết trước khi gọi, và đây là yêu cầu unload *sau khi* hook return, Zygisk
lo phần unmap.

### Cạm bẫy SONAME/ndk-build phát hiện ra

- ndk-build nhúng `LOCAL_MODULE` làm DT_SONAME: đổi `LOCAL_MODULE :=
rt_shim` thì loader sạch.
- CMake: `SONAME` property KHÔNG ăn (CMake tự append `-Wl,-soname,<output>`
  **sau** flag của ta → flag sau thắng). Giải pháp đúng: đổi luôn
  `OUTPUT_NAME "gcloudsync"` để soname mặc định = output name.
- Sau khi đổi OUTPUT_NAME phải dọn artifact cũ trong `build/readonly_native/`
  nếu build script còn tham chiếu tên cũ (đã cập nhật tất cả tham chiếu:
  build_zygisk.ps1, tests/arm64_machine.py, tests/test_readonly.py,
  tools/validate_*.py, docs).

### Java bridge

File mới `bridge/src/rt/internal/Bridge.java` thay cho
`bridge/src/cf/readonly/AimBridge.java` (đã xóa). `tools/build_bridge.py`
compile từ path mới. Payload `native/android_input.c` load class
`rt.internal.Bridge` và `RegisterNatives` với tên method mới
(read/status/active/apply — signature giữ nguyên).

### Deploy chú ý khi đổi module id

Module id mới **không tự thay** module cũ: `ksud module install` zip mới +
`ksud module uninstall cf_aim_speed_zygisk` (module cũ chỉ biến mất sau
reboot). Nếu để cả 2 cùng enabled → 2 loader cùng inject payload.

## 3. v2.2.6 — Fix EACCES(13): sepolicy.rule cho channel socket

### Chẩn đoán (vấn đề tồn tại từ 2.2.3)

- `connectCompanion()` chạy trong `preAppSpecialize` — lúc đó tiến trình
  **vẫn ở domain SELinux `zygote`** → socket được tạo với label
  `u:r:zygote:s0` và giữ label đó mãi (SELinux label theo đối tượng, không
  theo tiến trình).
- Sau specialization tiến trình thành `u:r:untrusted_app:s0:c25,c257,...`
  → mọi `send()/write()` lên socket đó bị deny → EACCES(13) →
  `cf_payload_input_fd()` fail → "root input companion unavailable (13)",
  runtime chạy passive (không swipe).
- **Không có AVC denial trong dmesg** (bị `dontaudit`) — không chẩn đoán
  được qua log kernel, phải bisect rule.

### Bisect (dùng `ksud sepolicy` live, không cần reboot)

```
ksud sepolicy check 'allow untrusted_app ksu unix_stream_socket {...}'   # rc=0: syntax ok
ksud sepolicy apply <file.rule>                                          # apply live
```

Lưu ý: `apply` chỉ nhận **file**, không nhận rule string.

- Rule `untrusted_app → ksu` (perm cụ thể lẫn `*`): **vẫn EACCES** → loại.
- Rule `untrusted_app → zygote` perm `{ read write sendto recvfrom getattr
  getopt setopt }`: **PASS** → xác nhận label thật là `zygote`.

### Fix vĩnh viễn: `module/sepolicy.rule`

```
allow untrusted_app zygote unix_stream_socket { read write sendto recvfrom getattr getopt setopt }
```

KernelSU/Magisk tự apply lúc boot. Quyền vừa đủ: read/recvfrom (response),
write/sendto (request), getopt (SO_COOKIE validation), setopt/getattr (fd
hygiene). Đã thêm vào staging trong `build_zygisk.ps1` và test zip content.

### Kết quả

```
SysLoader: control channel transferred          ← hết "unavailable (13)"
SysRuntime: input channel ready; verified       ← swipe input hoạt động lần đầu qua Zygisk
```

## 4. v2.2.7 — Anonymous remap (học từ AndKittyInjector `--hide` / Riru)

Nguồn: https://github.com/MJx0/AndKittyInjector — `KittyInjector.cpp`,
hàm `hideLibrary()`. Kỹ thuật gốc làm từ ngoài process bằng ptrace; ta đã
port thành **in-process** trong loader (dễ hơn nhiều vì đã ở trong process).

### 3 bước của `--hide` và mức độ áp dụng

| Bước gốc (KittyInjector) | Áp dụng? | Ghi chú |
|---|---|---|
| Gỡ soinfo khỏi solist/sonext của linker (patch `prev->next`) | **Chưa** | Cần offset soinfo per-Android-version; chỉ ảnh hưởng `dl_iterate_phdr` (tên lib vẫn liệt kê, không path) |
| Remap segment tại chỗ: backup → munmap → mmap(MAP_FIXED\|ANONYMOUS) → restore | **Đã** | Ý tưởng từ Riru; làm in-process |
| Randomize ELF header 64 byte tại base | **Đã** | Pseudorandom LCG xorshift-style, không ghi đè program headers phía sau |

### Hiện thực: `anonymize_mapped_image()` trong loader.cpp

Điểm khác so với bản gốc: **không đọc `/proc/self/maps`** — reconstruct
vùng mapping từ program headers của chính ELF (đã map):

1. `dladdr(symbol)` lấy base của image (symbol = `cf_payload_start_vm`).
2. Quét PT_LOAD → mảng region (start/size/prot), page-align floor/ceil.
3. PT_GNU_RELRO → split region chồng lấn thành read-only (giữ đúng layout
   r-x / rw- / r-- của linker, kể cả RELRO split). Capacity check mảng
   12 region.
4. Với mỗi region: malloc backup → memcpy ra → nếu region chứa base thì
   ghi đè 64 byte đầu (ELF header) bằng bytes pseudorandom → munmap →
   mmap MAP_FIXED|ANONYMOUS cùng địa chỉ → memcpy ngược → mprotect đúng
   prot gốc → `__clear_cache` nếu exec → free backup.
5. Gọi **trước** `cf_payload_input_fd()` và `cf_payload_start_vm()` —
   resolve mọi `dlsym` trước, anonymize, rồi mới chạy.

Tại sao an toàn: mọi con trỏ (code GOT, registered JNI natives, thread
entry) trỏ tới **địa chỉ** — remap giữ nguyên địa chỉ, chỉ đổi backing
file→anonymous. Payload không có PT_TLS, imports chỉ libc/liblog (34
symbols), BIND_NOW → không có lazy binding cần soinfo runtime.

### Kết quả verify (game pid 6627)

```
SysLoader: image detached from storage: 61440 bytes anonymous
maps grep rt_shim|gcloudsync|cfreadonly → NONE
payload giờ là: 75f6253000-75f625e000 r-xp 00000000 00:00 0   ← anonymous
```

Vùng anon exec trông giống vùng JIT của ART (game vốn có 5 vùng anon exec
khác) — không nổi bật. Runtime + input channel hoạt động đầy đủ sau remap.

### Điều kiện payload để kỹ thuật chạy ổn (đã kiểm tra bằng readelf)

- Không PT_TLS (nếu có sẽ phải tự quản TLS — không làm được kiểu này).
- 3 LOAD segment, no TLS, BIND_NOW (FLAGS/FLAGS_1 NOW).
- Imports: `__cxa_finalize, __cxa_atexit, __register_atfork, pthread_*,
  __android_log_print, __stack_chk_fail, sleep/usleep, getpid,
  process_vm_readv, memset/memcpy/memcmp/str*, __vsnprintf_chk,
  clock_gettime, calloc/free, fcntl, sendto/recvfrom, close, getauxval,
  __system_property_get, dl_iterate_phdr` — tất cả libc/liblog.

---

## Bề mặt lộ còn lại (residual tells) — theo thứ tự nguy cơ

1. **`dl_iterate_phdr` vẫn liệt kê tên `libgcloudsync.so`** (tên không có
   path, không maps, không header). Khắc phục: (a) mini-linker — tự mmap +
   relocation thủ công, không dlopen → không có soinfo; hoặc (b) port
   solist-unlink kiểu KittyMemoryEx (offset table per-SDK).
2. **`zn-zygisk-companion64 rt_shim`** trong ps (chỉ root thấy được nhờ
   hidepid; game không enumer được process khác — `/proc` mount
   hidepid=invisible).
3. **libzygisk.so của Zygisk Next** (3 dòng trong maps) — ngoài tầm kiểm
   soát của module; nếu cần triệt để phải đổi bản Zygisk khác/ReZygisk.
4. **FD socket đến companion** trong `/proc/self/fd` của game (socket:[ino])
   — vô danh, khó bắt, nhưng tồn tại.
5. **logcat SysLoader/SysRuntime/SysInput** — chỉ hiện khi debugging? Không,
   luôn ghi. Nếu muốn tắt hẳn: xóa LOGI ở bản release (giữ LOGE).

## Kiến thức device/môi trường (đã kiểm chứng, đừng lặp lại)

- **SUSFS `sus_map` KHÔNG hoạt động trên kernel này**: entry được kernel
  chấp nhận ("is flagged as AS_FLAGS_SUS_MAP") nhưng không filter maps —
  test controlled (self-read, child-reads-parent, uid 10281, on-disk +
  deleted) đều thấy. `sus_maps.txt` là dead weight; vẫn cập nhật path cho
  nhất quán nhưng không trông cậy. `sus_path` cũng không hide trong live
  test (test chạy qua ksu context, ít kết luận hơn). Reboot mới clear
  entries (không có CLI remove).
- `/proc` mount `hidepid=invisible` → game chỉ thấy process của mình.
- **logd có thể DROP line** khi game flood log lúc startup — luôn capture
  bằng `logcat -s <tags> > file` chạy streaming nền, đừng tin
  `logcat -d` một lần; và nếu thiếu line, chạy lại trước khi kết luận fail.
- SELinux: `getenforce` = Enforcing; game chạy `untrusted_app`; ZN companion
  chạy `u:r:ksu:s0`.
- `ksud sepolicy check '<rule>'` validate syntax; `ksud sepolicy apply
  <file>` apply live (mất khi reboot — chỉ dùng test; fix lâu dài là
  sepolicy.rule trong module).
- `ksud module install` đặt vào `/data/adb/modules_update/` — active sau
  reboot. Muốn chạy rule ngay sau khi install module (không reboot) thì
  apply thủ công file `/data/adb/modules/<id>/sepolicy.rule`.
- Game UID 10281; test với `su 10281 -c` (context thành ksu, không phải
  untrusted_app — chấp nhận cho test file access).
- ADB-over-TCP persist (`persist.adb.tcp.port=5555`) — tự reconnect sau
  reboot; sau reboot chờ `sys.boot_completed=1` + thêm ~15-20s cho ZN
  companion spawn.

## Quy trình build/deploy/verify chuẩn

```powershell
# Build (exit 0; dùng *> $null để tránh stderr-progress giả của PS)
powershell -ExecutionPolicy Bypass -File build_zygisk.ps1 *> $null
& tools\test-venv\Scripts\python.exe tests\test_readonly.py *> $null   # 32 tests

# Deploy: LUÔN push .sh ra /data/local/tmp rồi su -c 'sh ...' (tránh quoting hell)
adb -s 192.168.5.102:5555 push output\rt_shim.zip /data/local/tmp/
adb shell "su -c 'ksud module install /data/local/tmp/rt_shim.zip'"
adb shell "su -c 'reboot'"
# chờ boot + 20s, rồi verify:
```

```sh
# Verify script mẫu (streaming logcat, không tin logcat -d)
logcat -c; logcat -s SysLoader:V SysRuntime:V SysInput:V > /data/local/tmp/cap.log &
am force-stop com.vnggames.cfl.crossfirelegends; sleep 3
monkey -p com.vnggames.cfl.crossfirelegends -c android.intent.category.LAUNCHER 1
sleep 55; kill %1
cat /data/local/tmp/cap.log
grep -E "rt_shim|gcloudsync|cfreadonly" /proc/$(pidof com.vnggames.cfl.crossfirelegends)/maps
```

Log kỳ vọng (đầy đủ):
```
SysLoader: loader 2.2.x: target selected; image buffered (47568 bytes)
SysLoader: channel prepared: fd=6 exempt=0 fds_to_ignore=0
SysLoader: post specialization: image ready; channel verified=1 error=0
SysLoader: image detached from storage: 61440 bytes anonymous
SysLoader: control channel transferred
SysLoader: runtime loaded and started for ...; session copy anonymous
SysRuntime: runtime: bias=... build=a8793b51fee671e98de0cc0ad42bb85ffd5d0677 header=7f454c46
SysRuntime: resolver ready
SysRuntime: input channel ready; verified
```

## Files đã thay đổi trong các version này

- `loader/jni/loader.cpp` — unlink, DLCLOSE, anonymize_mapped_image(),
  toàn bộ log/identity scrub, sepolicy-aware flow
- `loader/jni/Android.mk` — LOCAL_MODULE := rt_shim
- `native/CMakeLists.txt` — OUTPUT_NAME "gcloudsync"
- `native/readonly_runtime.c` — tag SysRuntime + message scrub
- `native/android_input.c` — class rt.internal.Bridge + method names mới
- `bridge/src/rt/internal/Bridge.java` — mới (thay cf/readonly/AimBridge.java)
- `tools/build_bridge.py` — path mới
- `build_zygisk.ps1` — output rt_shim.zip, stage libgcloudsync.so,
  sepolicy.rule, lib path mới
- `module/module.prop` — id=rt_shim, version chain tới 2.2.7-anonimage
- `module/customize.sh` — perm cho payload/libgcloudsync.so, text scrub
- `module/sepolicy.rule` — MỚI (rule zygote socket)
- `module/README.md`, `README.md`, `docs/runtime-metadata.md` — cập nhật tên
- `tests/arm64_machine.py`, `tests/test_readonly.py`,
  `tools/validate_aim_payload.py`, `tools/validate_runtime_resolver.py` —
  tên artifact mới + assert sepolicy.rule trong zip
- Device: `/data/adb/susfs4ksu/sus_maps.txt` cập nhật path mới (cosmetic)
