# PHÂN TÍCH CHUYÊN SÂU `libLoader.so`
## Cấu trúc nhị phân, Cơ chế Hooking & Kỹ thuật vượt rào Anti-Cheat Tencent ACE (`libanort` / `libanogs`)

- **Đối tượng phân tích:** `lib/arm64-v8a/libLoader.so` (được trích xuất từ bản mod `CFL VNG.apk`).
- **Mục đích:** Nghiên cứu cơ chế an ninh tiến trình, phân tích kỹ thuật can thiệp bộ nhớ (Hooking/Patching) và nguyên lý đối đầu với hệ thống bảo vệ Tencent ACE (Anti-Cheat Expert).
- **Ngày lập:** 09/09/2026.

---

## 1. Thông tin tổng quan & Cấu trúc phân đoạn ELF

### 1.1. Thông số nhị phân
* **Định dạng:** ELF 64-bit LSB shared object, ARM aarch64, version 1 (SYSV).
* **Dung lượng:** 8.866.296 bytes (~8.45 MB).
* **Thuộc tính bảo vệ:** Bị làm rối (Obfuscated) bằng **OLLVM / Hikari**, mã hóa tên hàm và mã hóa chuỗi (String Encryption).

### 1.2. Bố cục các Section chính
| Tên Section | Loại | Địa chỉ nạp (VAddr) | Kích thước | File Offset | Chức năng kỹ thuật |
|---|---|---|---|---|---|
| `.hash` | SHT_HASH | `0x000190` | `0x01397c` | `0x000190` | Bảng băm symbol ELF |
| `.dynsym` | SHT_DYNSYM | `0x013b10` | `0x045720` | `0x013b10` | Chứa 11.500+ symbol mã hóa OLLVM |
| `.dynstr` | SHT_STRTAB | `0x059230` | `0x055a1c` | `0x059230` | Bảng chuỗi dynamic |
| `.rela.dyn` | SHT_RELA | `0x0aec50` | `0x009d20` | `0x0aec50` | Relocation bảng dữ liệu |
| `.rela.plt` | SHT_RELA | `0x0b8970` | `0x000450` | `0x0b8970` | Relocation bảng hàm gọi ngoại vi |
| `.plt` | SHT_PROGBITS | `0x0b8dc0` | `0x000300` | `0x0b8dc0` | Stub gọi thư viện hệ thống (libc, libdl...) |
| **`.text`** | SHT_PROGBITS | `0x0b90c0` | `0x0508ec` | `0x0b90c0` | Mã máy thực thi chính (~330 KB) |
| **`.main`** | SHT_PROGBITS | `0x1099ac` | `0x002680` | `0x1099ac` | **Section mã hóa chứa `JNI_OnLoad`** (~9.8 KB) |
| `.rodata` | SHT_PROGBITS | `0x10c030` | `0x2214d0` | `0x10c030` | Dữ liệu hằng số & chuỗi mã hóa (~2.2 MB) |
| `.init_array` | SHT_INIT_ARRAY | `0x39e780` | `0x000010` | `0x38e780` | Con trỏ Constructor giải mã tự động |
| `.data` | SHT_PROGBITS | `0x3a2000` | `0x4ded28` | `0x392000` | Vùng dữ liệu biến toàn cục (~5.1 MB) |
| **`.ced`** | SHT_PROGBITS | `0x880d28` | `0x000020` | `0x870d28` | **Metadata phục vụ giải mã section `.main`** |

---

## 2. Cơ chế tự bảo vệ & Giải mã Runtime (Self-Decryption Engine)

Khác với các thư viện thông thường, `libLoader.so` được bảo vệ bằng lớp mã hóa nhị phân nội bộ (ELF Section Packer):

```
[System Linker nạp libLoader.so]
               │
               ▼
   [.init_array gọi Constructor tại 0xb90cc]
               │
               ▼
   [Đọc thông số từ Section .ced (0x870d28)]
   ├── Target RVA: 0x1099ac (Section .main)
   └── Target Size: 0x002680
               │
               ▼
   [dladdr() tìm base address của chính nó]
               │
               ▼
   [mprotect(PROT_READ | PROT_WRITE | PROT_EXEC)]
               │
               ▼
   [RC4 Key Derivation (0xb9c98) + S-Box Init (0xd5320)]
               │
               ▼
   [Giải mã dòng dữ liệu mã máy của Section .main (0xd5580)]
               │
               ▼
   [mprotect(PROT_READ | PROT_EXEC) khôi phục quyền]
               │
               ▼
   [__builtin___clear_cache + dsb ish + isb (Đồng bộ CPU Cache)]
               │
               ▼
   [Hàm JNI_OnLoad tại 0x10b014 sẵn sàng thực thi]
```

### 2.1. Phân tích chi tiết Constructor tại `0xb90cc`
1. **Nạp Metadata `.ced`:**
   Tại `0xb90d0 - 0xb90d4`, con trỏ được trỏ tới `0x880d28` (Section `.ced`). Dữ liệu byte thực tế tại đây:
   `ac 99 10 00 80 26 00 00 09 0a 00 00 ...`
   - `0x1099ac`: Địa chỉ bắt đầu của section `.main`.
   - `0x002680`: Chiều dài section `.main`.
2. **Khởi tạo mảng hoán vị S-Box RC4 tại `0xd5320`:**
   Tại file offset `0xd5480`, mã máy nạp mảng hằng số định danh từ `0x00` đến `0xFF` (256 byte chuẩn của thuật toán khởi tạo S-Box trong mật mã RC4):
   `00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f ...`
3. **Mở quyền và giải mã:**
   Hàm gọi `mprotect` mở quyền `0x7` (`PROT_READ | PROT_WRITE | PROT_EXEC`), chạy thuật toán giải mã dòng RC4 trên toàn bộ dải `[0x1099ac ... 0x10c02c)`, sau đó hạ quyền về `0x5` (`PROT_READ | PROT_EXEC`).
4. **Đồng bộ hóa Instruction Cache:**
   Gọi `0x106208` (`__builtin___clear_cache`), kết hợp các lệnh đồng bộ phần cứng ARM64:
   - `dsb ish` (Data Synchronization Barrier Inner Shareable).
   - `isb` (Instruction Synchronization Barrier).
   Sau bước này, hàm **`JNI_OnLoad`** (tại `0x10b014`) chính thức hiện diện ở dạng mã máy sạch để hệ thống Android JNI gọi.

---

## 3. Động cơ Hooking & Memory Patching trong `.text`

Phân tích nhị phân chứng minh `libLoader.so` được trang bị sẵn một **Engine can thiệp mã máy runtime hoàn chỉnh** nằm trong khoảng địa chỉ từ `0xd4c94` đến `0xd8300`:

### 3.1. Bằng chứng 10 vị trí kích hoạt `mprotect` trong `.text`
Trong section `.text`, có 10 lệnh gọi `BL #0xb8fa0` (`mprotect`):
- `0xd4c7c`, `0xd5288`, `0xd52c4` (Phục vụ giải mã nội bộ `.main`).
- **`0xd71d4`, `0xd7280`, `0xd73f4`, `0xd7464`, `0xd74a0`, `0xd80ac`, `0xd8128`** (Phục vụ việc can thiệp, vá mã máy vào các thư viện bên ngoài).

### 3.2. Hàm MemoryPatch cốt lõi tại `0xd741c`
Hàm nhận 4 tham số: `(void* target_addr, size_t len, void* new_bytes, int prot_flags)`:
* **Căn chỉnh trang nhớ:** `and x22, x0, #0xfffffffffffff000` (lấy địa chỉ đầu trang 4KB).
* **Mở quyền ghi:** `mov w2, #7` $\rightarrow$ gọi `mprotect(page_start, page_size, PROT_READ | PROT_WRITE | PROT_EXEC)`.
* **Ghi đè mã mới:** Gọi `memcpy(target_addr, new_bytes, len)`.
* **Khôi phục quyền:** Gọi `mprotect(page_start, page_size, old_prot)`.

### 3.3. Bản chất Engine Hook: Thư viện có sẵn (Dobby/Substrate) hay Tự viết (Custom-built)?

Qua quét toàn bộ chuỗi, bảng symbol và cấu trúc mã máy của `libLoader.so`, có thể khẳng định:
> **`libLoader.so` KHÔNG sử dụng các thư viện hook có sẵn phổ biến như Dobby, Cydia Substrate, SandHook hay Frida — mà sử dụng một bộ công cụ Hook & Memory Patch TỰ VIẾT (Custom-built in-house).**

#### Vì sao không dùng Dobby hay Substrate?
* **Dấu vết chữ ký (Signatures):** Các thư viện nổi tiếng như Dobby chứa một bộ phân giải lệnh (Instruction Relocator dựa trên Capstone engine), cơ chế cấp phát Near-Trampoline đặc trưng, và các chuỗi định danh lộ liễu. Các Anticheat lớn như Tencent ACE (`libanort`, `libanogs`) đều có sẵn mẫu chữ ký để quét phát hiện ngay lập tức trong RAM.
* **Cồng kềnh không cần thiết:** Để bật/tắt các tính năng gian lận (như No Recoil, Silent Aim), modder không nhất thiết phải dùng đến dynamic trampoline phức tạp của Dobby mà chỉ cần cơ chế **Memory Patching** có thể hoàn tác (Toggleable Patching).

#### Cấu trúc Engine Hook tự viết được phát hiện trong `libLoader.so`:
1. **Class `MemoryPatch` tự xây dựng (Toggleable Patch Engine tại `0xd7b30 - 0xd8260`):**
   - Đoạn mã tại `0xd8184` kiểm tra một biến cờ trạng thái tại offset `[x0 + 0x110]` (`is_patched`):
     - Nếu `is_patched == 0`: Gọi hàm **`Modify()`** tại `0xd7b30` để áp dụng patch mã máy (5 điểm gọi hàm `0xd741c`).
     - Nếu `is_patched != 0`: Gọi hàm **`Restore()`** tại `0xd7e20` để khôi phục lại mã máy gốc từ mảng byte dự phòng (`backup_bytes`).
   - Cấu trúc C++ tương đương được tái dựng:
     ```cpp
     class MemoryPatch {
         void* target_address;   // [x0 + 0x08]
         size_t patch_size;      // [x0 + 0x58]
         uint8_t* new_opcodes;   // [x0 + 0x60]
         uint8_t* backup_opcodes;// [sp + 0x28]
         bool is_patched;        // [x0 + 0x110]
     public:
         bool Modify();          // 0xd7b30: mprotect(rwx) -> memcpy -> clear_cache -> mprotect(rx)
         bool Restore();         // 0xd7e20: mprotect(rwx) -> memcpy(backup) -> clear_cache
     };
     ```
2. **Module ELF Header Parser & PLT/GOT Hooking (`0xd4c94 - 0xd5100`):**
   - Tác giả tự viết hàm duyệt `Elf64_Phdr` (`e_phoff`, `e_phnum`) và tìm các segment `PT_LOAD` (loại `0x1`), `PT_DYNAMIC` (loại `0x2`).
   - Mục đích: Thay vì hook inline vào thân hàm, engine này tìm đến các con trỏ hàm trong bảng GOT của thư viện để tráo đổi (PLT/GOT Redirection). Cách làm này ít làm xáo trộn vùng `.text` hơn.
3. **Custom In-Memory ELF Loader (`0xd8e40`):**
   - Tại `0xd8e58`, `libLoader.so` gọi trực tiếp `mmap(..., PROT_READ, MAP_PRIVATE, fd, 0)` và tự mình duyệt các segment `PT_LOAD` để ánh xạ thư viện vào RAM mà **không thông qua hàm `dlopen()` chuẩn của hệ điều hành Android**.
   - **Mục đích:** Né tránh việc kích hoạt các hook giám sát `dlopen` mà `libanogs.so` thường đặt bẫy trong Bionic Linker.

---

## 4. Phân tích mục tiêu can thiệp đối với `libil2cpp.so`

### 4.1. So sánh: Đọc dữ liệu (Read-only) vs Can thiệp mã (Active Hooking)

| Chức năng Hack | Có cần sửa `.text` `libil2cpp.so`? | Cơ chế kỹ thuật thực tế |
|---|---|---|
| **ESP Box / Line / Distance / Radar** | ❌ **KHÔNG CẦN** | Chỉ cần **ĐỌC (Read-only)** dữ liệu trên Heap: Duyệt con trỏ `GWorld`, đọc mảng `Player.Transform.position`, ma trận `worldToScreenMatrix`. Render qua `eglSwapBuffers` của `libEGL.so`. |
| **Đồng bộ khung hình ESP (Anti-Jitter)** | ⚠️ **CÓ THỂ CẦN** | Để tránh lệch pha render giữa Cheat và Game, cheat thường hook vào hàm vòng lặp frame của Unity (`Camera.OnPreRender` hoặc `Player.Update`) để lấy tọa độ chuẩn từng frame. |
| **Silent Aim (Tâm không quay, đạn trúng đầu)** |  **BẮT BUỘC** | Không thể làm bằng Read-only. Bắt buộc phải **hook hàm tính đường đạn / hàm bắn** (ví dụ `FireRaycast`, `CalculateHitScan`, `SendShootPacket`) để đổi vector hướng đạn hướng thẳng về mục tiêu. |
| **No Recoil / No Spread (Không giật súng)** |  **BẮT BUỘC** | Game liên tục reset giá trị độ giật từ config. Cheat phải vá hàm cập nhật độ giật (`AddRecoil`) bằng opcode `RET` hoặc trả về 0 (`FMOV S0, WZR`). |
| **Bắn xuyên tường (Wall Penetration)** |  **BẮT BUỘC** | Hook hàm kiểm tra va chạm vật lý `Physics.Raycast` để bỏ qua các lớp vật cản (Layer Mask). |

> **Kết luận:** Sự hiện diện của cụm hàm `mprotect` + `memcpy` + `__builtin___clear_cache` trong `libLoader.so` khẳng định thư viện này **chắc chắn có thực hiện can thiệp sửa đổi mã máy**, phục vụ các tính năng can thiệp logic sâu như Aimbot, Silent Aim, No Recoil chứ không dừng lại ở mức ESP thụ động.

---

## 5. Cơ chế vượt qua hệ thống Anti-Cheat (`libanort.so` & `libanogs.so`)

### 5.1. Bản chất kiến trúc: Giới hạn cùng mức đặc quyền (User-Space Limitation)
Mọi cơ chế bảo vệ của `libanort.so` và `libanogs.so` đều chịu một giới hạn kiến trúc bất khả kháng:
* **Cùng chung một tiến trình:** Cả Game, Anticheat và Cheat Engine đều nạp chung vào một PID, chạy ở User Mode (Ring 3 / EL0 trên ARM64).
* **Không có phân quyền bộ nhớ nội bộ:** Hệ điều hành Linux/Android chỉ phân quyền giữa các tiến trình khác nhau (hoặc giữa Kernel và User Space). Bên trong cùng một tiến trình, bất kỳ luồng mã nào cũng có quyền đọc, ghi, gọi `mprotect` lên vùng nhớ của luồng khác.

```
┌────────────────────────────────────────────────────────────────────────┐
│                   ANDROID PROCESS MEMORY (User Mode / EL0)              │
│                                                                        │
│   ┌────────────────────┐                   ┌───────────────────────┐   │
│   │   libanort.so      │                   │     libLoader.so      │   │
│   │  (ACE Memory Guard)│                   │     (Cheat Loader)    │   │
│   │                    │                   │                       │   │
│   │ - Dispatcher quét  │ ◄────── PATCH ────┤ - dl_iterate_phdr     │   │
│   │ - Callback CRC     │    (Ghi đè RET)   │ - mprotect + memcpy   │   │
│   └─────────┬──────────┘                   └───────────┬───────────┘   │
│             │                                          │               │
│         Quét CRC                                   Can thiệp           │
│        (Bị vô hiệu)                               Opcode/Data          │
│             │                                          │               │
│             ▼                                          ▼               │
│   ┌────────────────────────────────────────────────────────────┐       │
│   │                       libil2cpp.so                         │       │
│   │                   (Game Logic & Unity)                     │       │
│   └────────────────────────────────────────────────────────────┘       │
│                                                                        │
│   ┌────────────────────┐                   ┌───────────────────────┐   │
│   │   libanogs.so      │                   │   Hook open() / read()│   │
│   │ (Environment Guard)│                   │   trên /proc/self/maps│   │
│   │                    │ ◄──── SPOOF ──────┤                       │   │
│   │ - Soi maps tìm rwx │   (Lọc bỏ Loader, │ - Trả về dữ liệu sạch │   │
│   │ - Soi module lạ    │    giả mạo r-xp)  │   như game chuẩn      │   │
│   └────────────────────┘                   └───────────────────────┘   │
└────────────────────────────────────────────────────────────────────────┘
```

### 5.2. Ba lớp kỹ thuật qua mặt chi tiết

#### ① "Đánh phủ đầu" vô hiệu hóa Dispatcher của `libanort` (Pre-emptive Patching)
* **Thứ tự thực thi:** `libLoader.so` được nạp qua `loadPluginByReflection` trong `AFMainActivity.onCreate`.
* **Định vị Anticheat:** Dùng `dl_iterate_phdr` (có trong bảng PLT của `libLoader.so`) để quét danh sách module trong tiến trình, lấy base address của `libanort.so`.
* **Vô hiệu hóa vòng quét:** `libanort.so` thực hiện kiểm tra CRC thông qua các hàm Dispatcher (như `FUN_001B1D40`, `FUN_001B27B0`, `FUN_001B3220` đã nêu trong tài liệu `libanort_reaction_analysis.md`). `libLoader.so` dùng `mprotect` mở quyền ghi và chèn ngay lệnh `RET` (`0xC0035FD6`) tại đầu hàm Dispatcher. Vòng lặp duyệt callback quét CRC bị ngắt từ gốc, khiến `libanort.so` không bao giờ kích hoạt được phép so sánh CRC với giá trị kỳ vọng `0xE27B8DAC`.

#### ② Giả mạo bản đồ bộ nhớ (`/proc/self/maps` Spoofing)
* **Mối đe dọa từ `libanogs.so`:** Daemon giám sát `libanogs.so` định kỳ đọc file ảo `/proc/self/maps` để phát hiện các trang nhớ có cờ `rwx` (dấu hiệu của inline hook) và tên file `libLoader.so`.
* **Cơ chế đối phó:** Trong binary của `libLoader.so` xuất hiện trực tiếp các chuỗi:
  ```text
  /proc/self/maps
  base.apk
  r-xp
  ```
  Kết hợp với các hàm import `open`, `read`, `close`. `libLoader.so` can thiệp vào các hàm đọc file hệ thống này. Khi `libanogs` mở `/proc/self/maps`, bộ lọc của `libLoader` sẽ xóa dòng chứa `libLoader.so` và đổi cờ các trang nhớ đã can thiệp từ `rwx` về `r-xp` chuẩn của `base.apk`.

#### ③ Cơ chế Bộ nhớ bóng (Shadow Memory / Page Fault Redirection)
* Nếu cần giữ cho `libanort` vẫn đọc được bộ nhớ sạch mà không bị crash:
  * Trang code thực thi trong `libil2cpp.so` được đặt ở chế độ hạn chế truy cập (`PROT_NONE` hoặc chỉ `PROT_EXEC`).
  * Khi CPU của game chạy đến: Mã hook được thực thi bình thường.
  * Khi luồng quét của `libanort` cố gắng đọc byte (`LDR`) để tính CRC: Hệ thống phát sinh ngoại lệ `SIGSEGV`.
  * Custom Signal Handler của cheat bắt lấy ngoại lệ, tạm thời chuyển con trỏ đọc sang bản sao sạch (Clean Backup) để `libanort` tính toán ra đúng mã CRC gốc, sau đó khôi phục lại.

---

## 6. Bài học & Định hướng phòng chống gian lận (Anti-Cheat Design)

Nghiên cứu này làm sáng tỏ các nguyên lý quan trọng trong thiết kế hệ thống bảo vệ trò chơi:

1. **Sự bất khả thi của Client-Side Anticheat thuần User-Space:**
   Không thể tin cậy bất kỳ cơ chế kiểm tra nào nếu mã kiểm tra và mã gian lận chạy cùng quyền trong cùng một tiến trình.
2. **Vai trò bắt buộc của Hardware / OS-Level Attestation:**
   - Cần triển khai **Google Play Integrity API** (Hardware-backed key attestation) để từ chối dịch vụ ngay từ cổng đăng nhập nếu APK bị re-sign (phát hiện chữ ký `MEMECHEAT.RSA`).
   - Kỹ thuật fake chữ ký như `ApkSignatureKillerEx` (`classes9.dex`) chỉ lừa được các hàm Java `PackageManager` cục bộ trong máy, hoàn toàn bất lực trước chứng thực Play Integrity gửi trực tiếp từ máy chủ Google.
3. **Chuyển dịch trọng tâm sang Server-Side Validation:**
   - Toàn bộ logic bắn, sát thương, tốc độ di chuyển và độ giật súng phải được máy chủ thẩm định (Server Reconciliation / Authority) thay vì giao phó cho Client tính toán.
