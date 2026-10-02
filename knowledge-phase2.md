# Knowledge — Phase 2: làm lại phương pháp actuation của aim (2.2.7 → 2.3.0-touchstream)

Tài liệu này ghi lại giai đoạn 2: thay cách "kéo input" của aim từ swipe rời rạc
sang **control loop liên tục**, cùng toàn bộ dẫn chứng từ tài liệu tham khảo và
trạng thái dừng hiện tại (đang có 1 crash chưa xử lý xong — xem mục cuối).

> File liên quan: `knowledge.md` (phase 1 — anti-detection: diskless, scrub,
> sepolicy, anonymous remap). Phase 2 KHÔNG đụng vào những cơ chế đó — mọi thứ
> vẫn hoạt động (boot log verified: image anonymous, maps sạch, channel OK).

---

## 1. Vấn đề: aim bị "đơ" — chẩn đoán kiến trúc cũ

Pipeline cũ (2.2.x), mỗi lần chỉnh:

```
Bridge tick 16ms → tính delta 1 lần từ snapshot → gửi CfSwipeCommand qua socket
→ companion fork+exec "/system/bin/input touchscreen swipe x1 y1 x2 y2 120"
(spawn process ~50-100ms + InputManager nội suy tuyến tính 120ms)
→ UP → cooldown nextStroke = +500ms → lặp
```

Ba lỗi结构性:

1. **~2 lần chỉnh/giây**, mỗi lần là cú stroke 120ms tính từ MỘT snapshot cũ —
   trong suốt stroke không đọc feedback gì (blind stroke). Target di chuyển →
   overshoot/miss.
2. **`fork+exec` mỗi stroke**: `/system/bin/input` là app_process — spawn chậm
   và để lại process liset vô nghĩa trên hệ thống.
3. **Tự.Cancel chính mình**: proxy `dispatchTouchEvent` quan sát MỌI touch
   (kể cả stroke của chính module — comment cũ ghi rõ "including channel
   strokes") → mỗi swipe của module cũng set `lastTouch` → một nửa cooldown
   500ms là **tự gây ra**.

Kết quả cảm nhận: staircase aim, "bắn bi", không theo kịp target di động.

## 2. Tài liệu tham khảo (dẫn chứng)

### 2.1 TaFFe.dev — "Aimbot Smoothing Using a Proportional Control"
https://taffe.dev/posts/aimbot-smoothing-using-a-proportional-control/

Bài kinh điển đúng về vấn đề này. Trích ý chính:

> "In conventional aimbot implementation for legitbots, you're working off the
> basis of a constant smoothing rate, causing the aimbot to slack behind moving
> targets or otherwise look super unnatural if the smoothing ends up being too
> low or even oscillate around the target."

Công thức P-controller trong bài (C#, đã chuyển ý sang Java trong Bridge):

```csharp
var errorx = _target.X - _position.X;
var xVelocity = errorx * Gain * _time * 100.0 + _newErrorX;   // + carryover
var xSpeed = Math.Clamp(xVelocity / dt, -MaxSpeedX, MaxSpeedX);
var targetPointX = (int) Math.Round(xVelocity);
_newErrorX = xVelocity - targetPointX;    // GIỮ LŨY THỪA SUB-PIXEL
```

Điểm mấu chốt ta áp dụng:
- **velocity ∝ error** (tỉ lệ với độ lệch), clamp MaxSpeed;
- **sub-pixel error carryover** — pixel là số nguyên; không tích lũy phần dư thì
  lỗi nhỏ không bao giờ được sửa (aim khựng lại cách target vài px);
- scale theo dt để độc lập framerate;
- reset error khi mất target (Tick/Clear).

### 2.2 hzqst/CS2_External — AimBot.hpp (mẫu external phổ biến)
https://github.com/hzqst/CS2_External — file AimBot.hpp (raw đã đọc, commit 5c312616):
https://github.com/hzqst/CS2_External/blob/5c312616/CS2_External/AimBot.hpp
(wiki: https://deepwiki.com/hzqst/CS2_External/8.2-aimbot-system)

```cpp
Yaw   = Yaw   * (1 - Smooth) + Local.Pawn.ViewAngle.y;   // Smooth 0.0..0.9
Pitch = Pitch * (1 - Smooth) + Local.Pawn.ViewAngle.x;
// ... FOV gate: if (Norm > AimFov) return;
```

Bài học: PC aimbot chạy **mỗi frame** với một bước nhỏ về phía target (60+ lần
chỉnh/giây) — không có khái niệm "stroke". FOV gate của nó tương ứng radius
200px của ta. (Nó ghi thẳng viewangles vào memory — ta không làm điều đó, ta
phải đi qua touch, nên bài học áp dụng ở tầng *nhịp điều khiển*, không phải
cách ghi.)

### 2.3 kurtdekker — Unity screen-space auto-aim helper (gist)
https://gist.github.com/kurtdekker/be2a15216a41bef5ad39b609ff1625ef

Aim-assist kiểu console ("nam châm"): lực hút = screenDelta × effectiveness,
với effectiveness = InverseLerp(activeRadius → 0, distance) — **lực tăng dần
khi càng gần target**, kèm floor/attenuation theo range. Cảm nhận: tự phanh
gần đích, không overshoot, không dao động quanh deadzone. Áp dụng thành
near-target easing trong Bridge (NEAR_RADIUS/NEAR_FLOOR).

### 2.4 openstf/devicefarmer minitouch — tầng inject tần số cao
https://github.com/openstf/minitouch — wiki: https://deepwiki.com/openstf/minitouch/1-overview

minitouch = chuẩn mực inject touch: một native daemon giữ thiết bị input, client
gửi lệnh text qua socket:

```
d <contact> <x> <y> <pressure>   # down
m <contact> <x> <y> <pressure>   # move  (60-120Hz, mỗi lần vài px)
u <contact>                      # up
c                                # commit (SYN)
```

Protocol header trả về khả năng thiết bị: `v 1`, `^ max_contacts max_x max_y
max_pressure`, `$ pid`. Bài học: **stream d/m/u/c thay vì swipe rời rạc** —
latency mỗi event = microseconds. Ta không dùng nguyên binary minitouch (nó
tạo uinput device mới) nhưng lấy đúng mô hình lệnh DOWN/MOVE/UP.

### 2.5 Nguồn đã loại bỏ (đừng mất công lại)
- `ExploitTheLoop/PUBGMTouchSimuAim` — chỉ README shell, không có code thật.
- Search UnknownCheats "mobile aimbot smooth touch drag" — kết quả rác (bing).
- Đường `input` command cho tần số cao — không thể: mỗi lần gọi = spawn
  app_process mới (chính là nút thắt của bản cũ).

## 3. Quyết định kiến trúc: inject vào node panel THẬT (không tạo uinput device)

Hai phương án cân nhắc:

| | (a) uinput virtual device (minitouch style) | (b) **ghi vào node panel thật** (đã chọn) |
|---|---|---|
| Thiết bị mới xuất hiện | CÓ — thấy trong InputDevice list / /proc/bus/input | **KHÔNG** — event trông y hệt firmware |
| MotionEvent deviceId | khác panel thật | **trùng panel thật** |
| Va chạm slot với ngón thật | không (device riêng) | slot 9 (panel 10 slot), tracking id riêng |
| BTN_TOUCH | tự quản | **phải phát 1/0** — nếu bỏ, Android coi slot là hover thay vì touch |
| Rủi ro | game có thể enumerate device | firmware palm-reset có thể hủy contact (xử lý được: proxy thấy pointer biến mất → disengage sạch) |

Vì mục tiêu số 1 của dự án là anti-detection → chọn (b). Event của ta đi qua
cùng một `/dev/input/eventX` — về mặt kernel/reader **không thể phân biệt** với
contact của firmware (cùng device, cùng protocol; điểm khác duy nhất là slot 9
và tracking id 23063 — nằm trọn trong range hợp lệ của panel).

### 3.1 Dẫn chứng recon trên device (getevent -lp, /proc/bus/input/devices)

- Panel thật: **`NVTCapacitiveTouchScreen`** (event2), protocol B:
  - `ABS_MT_SLOT 0..9` (10 slot), `ABS_MT_TRACKING_ID 0..65535`
  - `ABS_MT_POSITION_X 0..18400`, `ABS_MT_POSITION_Y 0..29440` — **portrait
    native, raw = display px × ~10** (1840/2944)
  - `ABS_MT_TOUCH_MAJOR 0..255`, `ABS_MT_PRESSURE 0..1`, có BTN_TOUCH
- `wm size` = 1840x2944 (portrait); game landscape = 2944x1840 (cùng panel xoay)
- SDK 36 (Android 16), ROM Lenovo ZUXOS
- `/dev/uinput` label `uhid_device` — ROM **đã có sẵn** một uinput device
  (`uinput_nav`) → uinput không phải anomaly trên máy này, nhưng (b) vẫn
  ẩn hơn nên chọn (b)

### 3.2 Dẫn chứng kernel chấp nhận ghi từ context root (Phase 1 test — PASSED)

Script `sendevent` ghi chuỗi protocol-B vào event2, `getevent -lt` echo lại
(chứng tỏ kernel nhận, SELinux ksu-domain cho phép ghi `input_device`):

```
EV_ABS ABS_MT_SLOT          00000009
EV_ABS ABS_MT_TRACKING_ID   00005a17
EV_ABS ABS_MT_POSITION_X    00002b1c      (= 11036 = raw của anchor 1943,736 layout0/rot1)
EV_ABS ABS_MT_POSITION_Y    00004be4      (= 19428)
EV_ABS ABS_MT_TOUCH_MAJOR   0000000c
EV_ABS ABS_MT_PRESSURE      00000001
EV_SYN SYN_REPORT           00000000
... MOVE (chỉ POSITION_X mới xuất hiện — input core tự dedup slot lặp)
... UP: ABS_MT_TRACKING_ID ffffffff
```

Quan sát hữu ích: **kernel input core dedup các giá trị ABS lặp** (ABS_MT_SLOT
9 viết lần 2 không echo) → companion cứ ghi slot mỗi frame, vô hại.

## 4. Hiện thực v2.3.0-touchstream

### 4.1 Protocol v2 (`native/input_protocol.h`) — thay CfSwipeCommand

```c
#define CF_TOUCH_MAGIC 0x43464934u
#define CF_TOUCH_SLOT 9
#define CF_TOUCH_TRACKING 0x5A17u          // 23063, range panel 0..65535
// action: 1=DOWN 2=MOVE 3=UP 4=RESET
// panel raw: X 0..18400, Y 0..29440; disp 2944x1840; anchor 1943,736; drift 420

typedef struct CfTouchCommand {
    uint32_t magic; uint8_t action, slot; uint16_t tracking;
    int32_t raw_x, raw_y;
} CfTouchCommand;   // 16 byte, pack khớp '<IBBHii'
```

- `cf_touch_valid()` (companion-side): magic/slot/tracking/action + raw trong
  range; UP/RESET yêu cầu raw = 0,0.
- `cf_touch_pack()` (runtime-side): **validate trong display-space** (giữ đúng
  tính chất bảo mật của bản cũ: DOWN phải đúng anchor 1943,736; MOVE trong hộp
  ±420 quanh anchor; app không thể ra lệnh touch tùy ý) rồi mới convert
  display→raw theo rotation (1/3) × layout (0/1). Hai hàm **không static** để
  host test gọi trực tiếp (mỗi binary chỉ có 1 TU include header).

### 4.2 Companion (`loader/jni/input_companion.c`) — hết fork/exec

- Tìm panel: quét `/dev/input/event0..63`, ioctl `EVIOCGNAME`, khớp
  `"NVTCapacitiveTouchScreen"` (không hardcode eventN — thứ tự node có thể đổi
  sau reboot), mở O_WRONLY, giữ fd.
- Mỗi lệnh = **một write() batched** struct input_event (time = 0 như sendevent):
  - DOWN (8 events): SLOT 9, TRACKING_ID 0x5A17, POS_X, POS_Y, TOUCH_MAJOR 12,
    PRESSURE 1, BTN_TOUCH 1, SYN_REPORT.
  - MOVE (3): SLOT 9, POS_X, POS_Y, SYN (tracking giữ nguyên theo protocol B).
  - UP/RESET (4): SLOT 9, TRACKING_ID −1, BTN_TOUCH 0, SYN.
- Reply int32 (0/errno) mỗi lệnh; giữ pattern one-in-flight của payload.
- **Cleanup bắt buộc**: client ngắt kết nối khi contact đang giữ → phát UP
  trước khi thoát — không bao giờ để "ngón ảo" kẹt trên màn hình.
- Chưa tìm thấy panel → mọi lệnh trả ENODEV(19) → payload tự đánh dấu channel
  hỏng (status 17).

### 4.3 Payload (`native/android_input.c`)

- JNI `apply(action, x, y)` (III)Z: validate display-space qua `cf_touch_pack`
  (rotation/layout lưu trong mutex) → gửi CfTouchCommand. Pattern pending-reply
  giữ nguyên từ bản swipe.
- JNI `calibrate(rotation, layout)` (II)Z: nhận rotation từ Display.getRotation()
  (chỉ nhận 1/3 — game luôn landscape) và layout mounting.
- `cf_payload_input_fd` KHÔNG đổi (handshake 0-byte probe giữ nguyên) → test
  descriptor cũ vẫn pass.

### 4.4 Bridge.java — control loop (trái tim của phase 2)

Vòng 16ms (~62Hz), các hằng số tune nằm đầu file:

```java
GAIN = 0.12f          // phần error sửa mỗi tick — CỐ Ý THẤP: loop đóng qua
                      // game frame nên trễ ~3-4 tick; gain cao sẽ dao động
MAX_STEP = 16f        // px/tick (~1000 px/s) — chặn tốc độ tối đa
NEAR_RADIUS = 64f, NEAR_FLOOR = 0.35f   // easing gần đích (kurtdekker):
                      // gain *= 0.35 + 0.65*(dist/64) khi dist<64 → tự phanh
SETTLE_PX = 2.5f, SETTLE_TICKS = 12     // error < 2.5px trong ~192ms → UP
ENGAGE_PX = 26f       // không DOWN cho lỗi nhỏ (tránh DOWN/UP lắt nhắt)
DRIFT_MAX = 380f      // ngón ảo chạy xa anchor quá → UP rồi DOWN lại tại
                      // anchor (mô phỏng người chơi nhấc tay re-drag)
```

Luồng mỗi tick: gates cũ (realTouch 500ms, viewport 2944x1840, focus,
finishing) → `read()` snapshot → P-step (chỉ khi **frame mới** — sample[3] đổi
so với tick trước, tránh overdrive giữa 2 frame) → MOVE tuyệt đối vị trí ngón
ảo → settle/drift/UP như trên.

**Phân biệt contact của mình với ngón thật** (sửa lỗi tự-cancel của bản cũ):

- Tracking id evdev là HẰNG SỐ (0x5A17 = 23063), nhưng Android InputReader tự
  cấp MotionEvent pointerId (thường là 0 cho sole pointer), không bảo toàn
  tracking id. Bridge vì vậy học pointerId động từ DOWN đầu tiên ở anchor.
- Heuristic bổ sung "sole pointer": trước khi engage, gate 500ms đảm bảo không
  ngón nào đang chạm → pointer DUY NHẤT xuất hiện ngay sau DOWN của ta = của
  ta; event nhiều pointer hoặc id khác = người thật → cancel + UP ngay.
- Học pointer id từ DOWN đầu tiên + **kiểm tra điểm rơi**: DOWN phải rơi trong
  ±60px quanh anchor (1943,736). Sai → layout mounting sai → tự flip layout
  (0↔1) một lần rồi retry; flip lần 2 vẫn sai → khóa (status 19) — fail-safe,
  không bắn lung tung khi mapping sai.

### 4.5 Test host (tests/test_readonly.py — 36/36 PASS)

- CompanionTests viết lại (bỏ mock fork/waitpid): mock open/ioctl/write/send/
  read/strncmp — assert **đúng chuỗi event protocol B** cho DOWN/MOVE/UP,
  assert cleanup UP khi client ngắt giữa chừng, assert ENODEV khi không tìm
  thấy panel (64 node quét), assert lệnh invalid ngắt channel + release.
- TouchPackTests (mới): `cf_touch_pack` cho cả 4 tổ hợp rotation×layout,
  biên drift box ±420 (420 nhận, 421 từ chối), DOWN lệch anchor bị từ chối,
  rotation 0/2 và layout 2 bị từ chối, và invariant: mọi kết quả pack đều pass
  `cf_touch_valid`.
- Bài học mock: ioctl buffer là **x2** (không phải x1); mock `read` phải tôn
  trọng số byte được yêu cầu (ghi cả chunk vào buffer 16 byte trên stack đã
  smash return address → UC_ERR_FETCH_UNMAPPED — một bug test rất hay).

## 5. Kết quả đã verified trước khi dừng

- Host: build OK, 36/36 test pass (exit 0 xác nhận bằng `*> $null`).
- Phase 1 (kernel): sendevent stream vào event2 accepted + getevent echo (mục 3.2).
- Deploy 2.3.0 + reboot: boot flow sạch, hai lần launch liên tiếp:
  ```
  SysLoader: loader 2.3.0: target selected; image buffered (52456 bytes)
  SysLoader: post specialization: image ready; channel verified=1 error=0
  SysLoader: image detached from storage: 65536 bytes anonymous
  SysLoader: control channel transferred
  SysRuntime: resolver ready
  SysRuntime: input channel ready; verified
  ```
  (maps sạch như 2.2.7 — phase 1 mechanisms không bị phase 2 phá.)

## 6. Crash `calibrate`: đã sửa trên host trong v2.3.1-jni-contract

Game **crash-restart 2 lần liên tiếp** sau khi vào splash fullscreen. Dropbox:

```
java.lang.Error: FATAL EXCEPTION [main]
Caused by: java.lang.UnsatisfiedLinkError: No implementation found for
boolean rt.internal.Bridge.calibrate(int, int)
(tried Java_rt_internal_Bridge_calibrate and Java_rt_internal_Bridge_calibrate__II)
    at rt.internal.Bridge.calibrate(Native Method)
    at rt.internal.Bridge.run(Bridge.java:225)     ← nhánh !calibrated trong run()
```

Nghịch lý được giải thích bởi một literal cũ: `JNINativeMethod methods[]` có
5 phần tử nhưng lời gọi là `RegisterNatives(..., methods, 4)`. ART đăng ký
thành công đúng 4 method đầu (`read/status/active/apply`), nên
`cf_android_input_start()` trả true và in log ready. `calibrate` ở vị trí thứ
5 không được bind, vì vậy chỉ crash khi Java gọi method này.

Bản sửa thay literal bằng `sizeof(methods) / sizeof(methods[0])`, nên số lượng
đăng ký luôn theo mảng. Test regression mới đọc class `rt.internal.Bridge`
trong chính DEX được nhúng, so toàn bộ native method name+signature với bảng
`RegisterNatives`, và kiểm tra `bridge_dex.h` khớp byte-for-byte với
`classes.dex`. Build hoàn tất, **37/37 test pass**.

### Device verify v2.3.1 (2026-09-11)

Đã cài ZIP qua KernelSU, reboot và xác nhận module active ở versionCode 231.
Hai lượt launch đều đi qua điểm crash cũ và đạt `resolver ready` +
`input channel ready; verified`; không còn `UnsatisfiedLinkError`/`FATAL
EXCEPTION`, maps vẫn sạch và companion còn sống. Ở lượt hai, PID đầu bị ROM
kill với reason `ZuiMemoryCleaner_Recents` sau khi activity mất top (không có
crash/Dropbox entry), sau đó PID mới khởi động bình thường và lại đạt channel
ready. PID cuối vẫn sống sau thời gian quan sát. Chưa verify chuyển động aim
trong training room.

### Actuation fix v2.3.2-touch-contact

Device probe trong training room chỉ ra chuỗi cũ xuất hiện ở app dưới dạng
`ACTION_HOVER_ENTER/HOVER_EXIT`, dù `getevent` thấy đủ ABS events. Nguyên nhân:
companion phát tracking/position/pressure nhưng không assert `BTN_TOUCH`.
InputReader vì vậy coi contact là hover; Bridge không bao giờ học được DOWN,
timeout sau 220 ms rồi retry nên camera không di chuyển.

v2.3.2 thêm `EV_KEY BTN_TOUCH=1` vào DOWN và `BTN_TOUCH=0` vào
UP/RESET/cleanup. Host test kiểm tra rõ cặp key event này ngoài toàn bộ sequence
protocol-B; **38/38 test pass**. Đã cài versionCode 232 và reboot. Probe mới
được app nhận đúng `ACTION_DOWN` tại khoảng `(1943,736)`, dịch đến
`(1965,736)`, rồi `ACTION_UP`, nên đường evdev → InputReader đã hoạt động.

Runtime trong training room hiện báo `live data: entries=0 frame=0 assist=1`:
ít nhất một trong hai aim mặc định (thường/sniper) của game vẫn bật, nên policy
cố ý trả no-target và chưa cho controller engage. Cần tắt cả hai trong game rồi
verify tiếp; không nên tune GAIN/MAX_STEP trước khi `assist=0`.

## 7. File đã thay đổi trong phase 2

- `native/input_protocol.h` — viết lại toàn bộ (CfTouchCommand, valid, pack)
- `loader/jni/input_companion.c` — viết lại toàn bộ (evdev stream, hết fork/exec)
- `native/android_input.c` — swipe → apply(III) + calibrate(II), JNI table 5 mục
- `bridge/src/rt/internal/Bridge.java` — viết lại toàn bộ (P-controller)
- `tests/test_readonly.py` — CompanionTests mới + TouchPackTests mới
- `module/module.prop` — 2.3.2-touch-contact / 232
- `loader/jni/loader.cpp` — log version 2.3.2
- Script chẩn đoán: `output/recon_input.sh`, `output/test_panel_write.sh`,
  `output/verify_boot.sh`, `output/check_crash.sh`

## 8. Việc tiếp theo khi quay lại (checklist)

1. Vào training room: tune GAIN/MAX_STEP theo cảm giác (mục 4.4 là
   điểm khởi đầu bảo thủ).
2. Verify aim end-to-end: log `SysInput: contact <id> at 1943,736` (learn OK),
   status 14/16, camera bám bot mượt; nếu log "contact landed at ...; layout"
   → mapping đang lệch, xem layout flip.

## 9. Sniper co-aim v2.4.x (thay thế mục 4.4 và 8)

### Đo trigger trực tiếp

Probe 2.3.3 resolve và lấy mẫu `m_EnableAimAssistance`,
`m_EnableAimAssistanceForSniper`, `m_DoingAimAssist` và
`m_CurrentAimAssistTarget` từ local controller. Trên đúng device/build đã ghim:

- projection Y khi đóng scope ổn định gần `2.112`;
- sniper scope mở hoàn toàn có projection Y `5.671`;
- khi game chọn enemy: `doing=1`, current-target khác null và con trỏ đó khớp
  chính xác một candidate đã validate (`listed=1`);
- `doing` thường rung 0/1 vài frame trong khi target không đổi, do đó đây là
  tín hiệu cạnh trigger chứ không phải gate điều khiển liên tục.

### v2.4.0-sniper-copilot

Đã bỏ policy chọn enemy gần nhất/range 200 px và gate bắt buộc aim mặc định phải
tắt. Game quản lý chọn target và range; native chỉ nhận đúng
`m_CurrentAimAssistTarget`, sniper scope mở hoàn toàn (`projection_y >= 5.30`),
snapshot mới/coherent và enemy sống/visible.

Bridge chạy state machine `ARMED -> ACTIVE -> LATCHED` ở cadence 8 ms. Mỗi MOVE
tối đa 4 px (`GAIN=.20`) và gửi UP ngay khi error <=3.5 px. Vận tốc target được
tính trong world-space, clamp 12 unit/s, lọc EMA và lead 55 ms trước khi chiếu.
Nhờ vậy chuyển động camera không bị nhầm thành vận tốc target. Host đạt 39/39
test; versionCode 240 đã được cài và chạy trên device.

### v2.4.1-chest-trigger-buffer

Trải nghiệm device lộ hai lỗi. Với target rất gần, cạnh game-assist có thể kết
thúc ngay trong animation mở scope, trước khi projection đạt 5.30. Bridge hiện
buffer target do game chọn trong 300 ms và chỉ dùng khi scope mở hoàn toàn;
native chỉ được giữ lại đúng target gần nhất đó sau khi game xóa pointer, không
được tự chọn target khác.

`m_CachedUpperBodyTransform` đo được cao hơn pawn root khoảng 0.78 world-unit
(socket sát đầu khoảng 0.95), giải thích việc tâm nằm ở vai/đầu. Snapshot mới
dùng điểm 70% trên đoạn root→UpperBody, xấp xỉ giữa ngực. Prediction dọc được
clamp, lọc mạnh hơn và chỉ áp dụng 20% lead ngang để loại nhịp nhún animation.

`LATCHED` không còn re-arm do nhiễu `doing` false/true trong cùng scope; chỉ đóng
scope hoặc game đổi target ổn định mới bắt đầu cycle khác. Test mới bao phủ việc
giữ target qua trigger ngắn và phép nội suy giữa ngực. Build đạt **41/41 test**;
versionCode 241 đã cài, boot, đạt `resolver ready` và
`input channel ready; verified`. Cycle đầu quan sát trên device là đúng một
`ACTIVE -> LATCHED` trong 477 ms, không tự restart ngay trên cùng target.

## 10. Gyro takeover v2.5.x

### Đường điều khiển đã xác nhận trên device

`libunity.so` gọi Android NDK sensor API trực tiếp. Payload chỉ đổi GOT slot có
quyền ghi của `ASensorEventQueue_getEvents`, luôn gọi hàm gốc rồi cộng hiệu chỉnh
vào hai trục X/Y của event gyro type 4. Không sửa text page, không đổi permission
page và không đụng accelerometer/gravity. Custom touch bị tắt hoàn toàn khi chế
độ gyro takeover chạy.

Đo thực tế xác nhận camera-right tương ứng sensor X âm và camera-up tương ứng
sensor Y âm. Một phát hiện quan trọng khác: khi tắt
`m_EnableAimAssistanceForSniper`, gyro vật lý cũng không còn quay camera dù Unity
vẫn nhận raw sensor event. Vì game dùng chung downstream gate cho sniper assist
và gyro, flag sniper phải luôn bật; payload không patch hay vô hiệu hóa flag này.

### v2.5.1-gyro-takeover-lowgain

Game tiếp tục chọn target và range. Cạnh `doing + current target` được buffer
300 ms trong lúc scope mở, sau đó controller khóa đúng target đã validate. Khi
synthetic gyro bắt đầu điều khiển, `doing` và current-target của game có thể rơi
về false/null; lock vẫn được giữ qua grace ngắn để takeover không tự ngắt.

Controller ban đầu dùng `Kp=5.0`, cap `0.12 rad/s`, smoothing 25%, deadband
3.5 px, resume 8 px, prediction 55 ms và điểm ngắm 70% root→UpperBody. Device
test xác nhận target đứng yên hoạt động đúng và gyro cho chuyển động mượt hơn
touch, nhưng target chạy bị đuổi hơi chậm và tâm nằm gần giữa bụng.

### v2.5.2-gyro-follow-tuned

Log live cho thấy một số lần đọc transform race trong đúng một pass 8 ms. Worker
cũ lập tức đặt correction về 0 ở mỗi pass lỗi, tạo chuỗi add khác 0/0 xen kẽ và
làm giảm tốc độ bám thực tế. Bản mới giữ lệnh đã validate gần nhất tối đa 8 pass
(64 ms); sau đó vẫn fail closed, đồng thời consumer có expiry cứng 100 ms.

Các thông số được tune có kiểm soát:

- prediction ngang: `55 ms → 90 ms`;
- proportional gain: `Kp 5.0 → 5.5`;
- rate cap mỗi trục: `0.12 → 0.15 rad/s`;
- điểm aim: `70% → 78%` trên đoạn root→UpperBody để nâng từ bụng lên giữa ngực;
- giữ nguyên smoothing 25%, deadband 3.5 px và resume 8 px để hạn chế overshoot.

Build đạt **45/45 test**, SHA-256 gói ZIP là
`C3BB1CD2EEBE6434103A19C4959EAA2F42F14FEDF21A8D5AB987E9534F60F017`.
VersionCode 252 đã cài trên `192.168.5.102:5555`, reboot thành công; module active
báo đúng `2.5.2-gyro-follow-tuned`, game PID mới khởi động, resolver và gyro
filter đều ready, không có fatal/crash trong lần kiểm tra khởi tạo.

### v2.5.4-pvp-controller

Triệu chứng: cùng module hoạt động trong đấu tập nhưng trận team PVP hoàn toàn
không có correction. Probe 2.5.3 xác nhận gyro hook vẫn nhận raw event, nhưng
scene luôn dừng ở `no-local`: target list có đủ `10/10` pawn và `10/10`
PlayerInfo, trong khi không controller nào qua exact-class profile.

Frida metadata read-only trên đúng trận cho thấy local pawn ở slot 0 dùng
`WNPVPGame.WNTeamGame.TeamGamePlayerController`; controller này trỏ ngược đúng
pawn và có LocalPlayer hợp lệ. Nó kế thừa `PVPPlayerController`, nhưng payload
cố ý chỉ chấp nhận class pointer đã resolve trước và allowlist chưa chứa subclass
team-game này. Báo cáo được lưu tại
`output/runtime_metadata/pvp-6302-diagnostic.json`.

v2.5.4 thêm đúng `TeamGamePlayerController` vào profile controller, không nới
lỏng kiểm tra class động và không thay thuật toán gyro. Log xác minh sau cài:

- 279/279 mẫu gyro có frame scene khác 0;
- projection scope đạt `5.671`, `doing + game_target` xuất hiện;
- target được khóa và `listed=1`;
- có 6 mẫu sensor nhận correction khác 0, ví dụ
  `add=(-0.12111,-0.04717)` với error `(108.5,-35.9)`;
- controller đạt settled gần tâm và không có fatal/crash.

Build đạt **46/46 test**; versionCode 254 đã cài và xác minh end-to-end trong
trận team PVP trên device.

### v2.5.5-modes-flag-policy

Scene-reader bổ sung exact-class profile cho đặt bom:
`WNPVPGame.WNBombGame.BombGamePlayerPawn` và
`WNPVPGame.WNBombGame.BombGamePlayerController`. Chế độ này tiếp tục dùng
semantics phe `camp 1/2` như team PVP.

Đấu đơn 1v9 được nhận diện fail-closed bằng exact class của `m_Game`:
`WNIndivdualGame.IndivdualGame`. Chỉ trong mode này, mọi pawn có ID khác local
được coi là enemy dù toàn bộ player cùng mang `camp=1`; các mode còn lại không
bị nới lỏng kiểm tra phe.

`m_EnableAimAssistanceForSniper` trở thành flag chọn policy. Khi flag bật,
trigger vẫn yêu cầu scope sniper mở hoàn toàn (`projection_y >= 5.30`) cùng với
`m_DoingAimAssist && m_CurrentAimAssistTarget != null`, có buffer 300 ms trong
animation mở scope. Khi flag tắt, cùng trigger `doing + target` được chấp nhận
cho mọi loại súng mà không cần scope gate. Sau khi capture, target vẫn được giữ
theo state machine hiện có để gyro takeover không tự ngắt khi game nhả trigger.

Build đạt **50/50 test ARM64**; versionCode 255. ZIP cuối có SHA-256
`9892F4574C9AF7633C7CCEC213FC8A6877C4C4A79828D719D7C670291D3CEEBD`.
Bản này đã được cài trên `192.168.5.102:5555`; sau reboot log xác nhận đúng
`loader 2.5.5`, `resolver ready` và `gyro filter ready`, không có crash.

### v2.5.6-universal-release

Policy không còn phân nhánh theo `m_EnableAimAssistanceForSniper` và không dùng
scope/projection để gate. Mọi loại súng chỉ có một trigger acquisition:
`m_DoingAimAssist && m_CurrentAimAssistTarget != null`. Game tiếp tục tự chọn
target/range; tùy chọn gyro trong game là công tắc tổng ở phía người dùng.

Mỗi cycle giữ target đã capture qua lúc game nhả trigger tạm thời. Khi sai số
đạt 3.5 px, correction về 0 và cycle chuyển sang latched. Cùng target chỉ re-arm
sau khi trigger đã nhả ổn định 120 ms; target khác được nhận ngay. Một cycle
đang kéo có fail-safe cứng 2 giây.

Release build loại toàn bộ log `SysLoader`, `SysRuntime`, `SysGyro`, bỏ import
`__android_log_print`, bỏ luôn vòng tổng hợp log 100 ms trong sensor hook. Khi
không có correction, wrapper trả ngay sau hàm sensor gốc mà không quét event.
Đấu đơn 1v9 và đặt bom vẫn giữ các fix exact-class của v2.5.5.

Build đạt **53/53 test ARM64**; versionCode 256. ZIP release có SHA-256
`D67742AAAE7FEFB9213B94D51F147CC208CE27459EB00400CECC759FA2A5EBBD`.
Bản này đã được cài lên `192.168.5.102:5555` và reboot thành công. Hash loader
và payload trên thiết bị khớp byte-for-byte với staging; game nạp payload,
worker trả trạng thái `CF_WAITING_SCENE` tại lobby, không có crash và không phát
sinh dòng log module nào. Frida chỉ dùng để đọc getter trạng thái rồi đã tắt.

### v2.5.7-sniper-idle-gate

Policy quay lại sniper-only. Một cycle mới chỉ được phép bắt đầu khi đồng thời có
`m_EnableAimAssistanceForSniper = true`, scope đã mở hoàn toàn
(`projection_y >= 5.30`) và
`m_DoingAimAssist && m_CurrentAimAssistTarget != null`. Đóng scope hoặc tắt flag
sniper reset controller và correction về 0; các loại súng thường không còn kích
hoạt custom gyro.

Scene reader được tách thành hai tầng để tránh quét lãng phí. Sau lần bind đầy đủ
đầu tiên, idle loop 8 ms chỉ xác minh cache pawn/controller/local-player, đọc ba
flag aim, current target và đúng một hệ số `projection[5]`. Danh sách attackable,
toàn bộ transform và hai ma trận camera chỉ được đọc khi có trigger sniper mới
hoặc một cycle đang tracking. Sau khi settled, cùng target không gây full scan
cho đến khi trigger nhả ổn định 120 ms; khi scope đóng thì chỉ còn poll nhẹ. Nếu
cache/scene chưa tồn tại (ví dụ ở lobby), full rebind được giới hạn còn 4 lần/giây
thay vì 125 lần/giây.

Release tiếp tục không có log runtime và sensor wrapper vẫn trả nhanh khi không
có correction. Các profile team PVP, đấu đơn 1v9 và đặt bom được giữ nguyên.
Build host đạt **55/55 test ARM64**; versionCode 257. ZIP cuối có SHA-256
`369709A729A5805ABB015EE3B3D8D30B5AF621A2D9AC243FD1A38A55B4EC42F5`.

Bản này đã được cài lên `192.168.5.102:5555` và reboot thành công. Module active
báo đúng `2.5.7-sniper-idle-gate`; hash loader
`e25ae04142320432dc9570a31fba5353db3001f9ee83c32d4e3ef7861aacf8fe` và
payload `bcdb8dfe106a34ab10cb6a89fdbeb64e9972aaa7f3158ad8cfcc492b3e5b7e8d`
trên thiết bị khớp staging. Game khởi động với PID mới, payload anonymous được
nạp và getter trả status 8 (`CF_INPUT_UNAVAILABLE`, đúng vì touch bridge bị tắt);
không có crash hay log module. Frida server và ADB forward đã được dừng/gỡ sau
phép đọc trạng thái.

### v2.5.8-direct-game-target

Theo kiểm chứng thực tế, `m_DoingAimAssist` về false ngay khi enemy bị che khuất.
Policy vì vậy chuyển sang game-authoritative: mỗi poll 8 ms đều yêu cầu sniper
flag, scope đầy đủ, `m_DoingAimAssist = true` và current target khác null. Chỉ
cần `doing` hoặc target mất là gyro correction tắt và lock reset ngay; không còn
giữ target qua trigger-drop hay chờ re-arm 120 ms. `m_IsPawnVisible` được bỏ khỏi
metadata và không còn tham gia policy.

Khi active, `cf_scene_read` không duyệt `m_AttackableTargetList`. Nó đọc trực
tiếp pawn do `m_CurrentAimAssistTarget` trỏ tới, vẫn kiểm tra exact pawn class,
khác local, chưa destroyed/hidden, health dương và hai transform hợp lệ, sau đó
chỉ dựng camera matrix và candidate duy nhất này. Full target-list scan chỉ còn
xảy ra ở lần bind/rebind local controller ban đầu; idle tiếp tục dùng poll nhẹ.

Build đạt **58/58 test ARM64**; versionCode 258. ZIP có SHA-256
`84856C56ED07F9BB0258D502F41CF63E48C2784195A09C912465476A6012CB66`.
Bản đã được cài lên `192.168.5.102:5555` và reboot thành công. Module active báo
đúng `2.5.8-direct-game-target`; hash loader
`e25ae04142320432dc9570a31fba5353db3001f9ee83c32d4e3ef7861aacf8fe` và
payload `dc870edade2f33a383dfbdcb8cd58e4c241fd82827d4e6daf6276b490884eac7`
khớp staging. Game PID 6275 nạp Unity/IL2CPP và anonymous payload mapping, không
có crash hay dòng log module; ZIP tạm trên thiết bị đã được xóa.

### v2.5.9-trigger-grace500

Phản hồi thực tế của v2.5.8 cho thấy `m_DoingAimAssist` đôi khi rơi về false khi
tâm mới tới eo và chưa đạt giữa ngực. Controller mới ghi thời điểm falling edge
đầu tiên và, chỉ khi cycle chưa settled, cho đúng locked pawn tối đa 500 ms để
hoàn tất. Grace kết thúc tại `>= 500000000 ns`; nếu Doing trở lại thì timer được
xóa, còn false→true có thể re-arm cycle tiếp theo ngay.

Trong grace vẫn giữ tối ưu direct-target: runtime truyền `locked_target` vào
`cf_scene_read_target`, chỉ đọc pawn đó cùng camera/transform, không quét danh
sách enemy. Scope đóng reset ngay. Pawn sai class, trùng local, destroyed,
hidden, hết máu hoặc transform/chest point không hợp lệ không nhận correction;
việc target invalid không được grace che giấu. `m_IsPawnVisible` tiếp tục không
được resolve hay sử dụng.

Build đạt **59/59 test ARM64**; test biên xác nhận active ở 499,999,999 ns và
reset đúng tại 500,000,000 ns. VersionCode 259, ZIP SHA-256
`AF887CEE8BD9695D90DE88CFB75BBE7AE4EFD74428B7BB33BF461D2BEEB03DE7`.
Bản đã cài và reboot trên `192.168.5.102:5555`; module active đúng
`2.5.9-trigger-grace500`. Hash loader
`e25ae04142320432dc9570a31fba5353db3001f9ee83c32d4e3ef7861aacf8fe` và
payload `c1d3ad09261b1b700246940b1c77561dcc069b1a3d7e057f7bdc9538e9696fb4`
khớp staging. Game PID 6095 nạp Unity/IL2CPP và anonymous payload, không crash
hay sinh log module; ZIP tạm đã được xóa.

### v2.6.0-adaptive-poll

Probe read-only trên build đã pin xác nhận `Pawn.get_CurrentWeapon()` ở RVA
`0x6876c98`. Khi cầm sniper, object trả về thuộc `WNGameBase.WNWeaponSniper`;
field trực tiếp `WNWeaponSniper.m_IsZooming` đổi chính xác từ 0 sang 1 khi mở
scope. Với súng thường, current weapon không assignable sang class sniper.
`WNPawn.m_IsInZoomingState` không đổi theo scope trong phép đo nên không được
dùng. Gate `projection_y >= 5.30` cũng được bỏ hoàn toàn; `projection_y` chỉ còn
tham gia phép chiếu sai số màn hình sang gyro.

Worker dùng ba cadence thích nghi. Khi chưa bind được local pawn trong trận,
scene rebind chạy mỗi **5000 ms**. Sau khi đã bind nhưng current weapon chưa phải
sniper, getter current weapon được gọi tối đa mỗi **500 ms**. Khi đã cache sniper,
worker chạy mỗi **8 ms**: scope đóng chỉ đọc trực tiếp `m_IsZooming` rồi return;
scope mở mới đọc `m_EnableAimAssistanceForSniper`, `m_DoingAimAssist`,
`m_CurrentAimAssistTarget` và hệ số projection. Transform cùng full camera matrix
chỉ được đọc khi trigger active hoặc đang hoàn tất grace 500 ms; fast path không
duyệt danh sách enemy. Phân loại current weapon vẫn refresh 500 ms ngay cả khi
worker đang ở nhịp 8 ms.

Build release đạt **60/60 test ARM64**; versionCode 260. ZIP SHA-256
`65DD5C409E55495E693610EB8435159D356EACDC722511FBF36BCC9947F7648B`.
Bản đã cài và reboot trên `192.168.5.102:5555`; module active đúng
`2.6.0-adaptive-poll`. Hash loader
`e25ae04142320432dc9570a31fba5353db3001f9ee83c32d4e3ef7861aacf8fe` và
payload `bdc742ce8832dcf7d2611ae3a21e0ea4a43c32b5b8effa6e41492f96cb769aec`
khớp staging. Game PID 5620 nạp Unity/IL2CPP, file payload tạm đã được unlink,
không còn tên module trong maps, PID ổn định và không có fatal/runtime log.

### v2.8.0-adaptive-lead

Giữ nguyên gate sniper gồm current weapon assignable từ `WNWeaponSniper`,
`m_IsZooming`, `m_EnableAimAssistanceForSniper` và trigger
`m_DoingAimAssist + m_CurrentAimAssistTarget`.

Prediction chuyển sang đo vận tốc ngang từ root transform, không còn lấy chuyển
động của điểm ngực pha root/upper-body làm vận tốc. Dưới `0.60 m/s` không lead;
từ `0.60-1.80 m/s` nội suy mượt đến horizon tối đa 90 ms và chỉ mở sau ba mẫu
cùng hướng. Khi đổi hướng hoặc giảm dưới ngưỡng, velocity cũ bị xóa ngay. Sau
world prediction còn có gate screen-space: dưới 4 px bỏ qua, 4-12 px blend dần
và từ 12 px dùng đầy đủ. Trục Y không prediction. Target có lead thực sự từ 1 px
trở lên không latch ở deadband 3.5 px mà tiếp tục được cập nhật trong cycle.

Scene binding bỏ whitelist class theo Team/Bomb/Individual/Training và bỏ camp
khỏi quyết định target. Pawn, controller và local player được xác thực bằng
assignability với base class cùng các liên kết hai chiều. Đường active tiếp tục
đọc thẳng pawn do game chọn, kiểm tra khác local, destroyed/hidden/health,
transform và target/trigger nhất quán; không quét hay tự chọn enemy trên tick
8 ms.

Build release đạt **69/69 test ARM64**; versionCode 280. ZIP SHA-256
`396CADF18157FFAD58CA9AE43E57657ED9F7EF45B564973E41DDAAD799EDE0EC`.
Bản đã cài và reboot trên `192.168.5.102:5555`; payload
`60e7c3798734436c593cd0f487c60cbbc0ee69cfca7a02e64abcd4555677e40c`
và loader `e25ae04142320432dc9570a31fba5353db3001f9ee83c32d4e3ef7861aacf8fe`
khớp ZIP. Game PID 6162 nạp Unity/IL2CPP cùng anonymous executable payload
0xc000 và crash buffer không có lỗi của game.

### v2.8.1-continuous-lead (build-only)

Prediction bỏ các vùng tốc độ `0.60/1.80 m/s`, gate ba mẫu và các vùng pixel
`4/12 px`. Horizon dùng một đường liên tục duy nhất:
`90 ms * clamp(filtered_speed / 12 m/s, 0, 1)`. Vận tốc vẫn đo từ root và chỉ
dự đoán ngang. EMA alpha 0.30 khởi tạo một hướng mới ở 30% raw velocity rồi hội
tụ dần; khi đổi hướng cũng bắt đầu nhỏ theo hướng mới thay vì giữ vector cũ hoặc
nhảy toàn lực trong một tick. Ngưỡng follow 1 px chỉ dùng để quyết định settle,
không chia mức prediction.

Build release đạt **69/69 test ARM64**; versionCode 281. Theo yêu cầu, bản này
chỉ được build thành `output/rt_shim.zip`, không push, không cài và không reboot
thiết bị.

## 13. Experimental hook_version — scope-open event

Live tracing on 2026-09-19, PID 5142, confirmed three consecutive scope-open
and scope-close cycles on the same `WNWeaponSniper` object. Every open called
`set_IsZooming(true)`, changed `m_IsZooming` from false to true, then entered
`T_OnZooming` in the same millisecond. Every close called
`set_IsZooming(false)` and left the field false; two completed in the same
millisecond and one completed after 18 ms.

Disassembly for pinned IL2CPP build ID
`a8793b51fee671e98de0cc0ad42bb85ffd5d0677` shows the open-only callsite at RVA
`0x889b64c`: after reading `m_IsZooming` at offset `0x8e8`, the false path skips
the call and the true path executes `BL WNWeaponSniper.T_OnZooming` (RVA
`0x889b680`). The experimental build patches only this aligned 4-byte BL after
validating its decoded destination, calls the original method, then increments
an atomic generation and wakes the worker.

`hook_version` therefore keeps the stable target/gyro logic but changes idle
scheduling. Scope open wakes immediately and forces a current-weapon refresh;
scope open continues at 8 ms, while scope closed sleeps indefinitely on the
event with no lobby/current-weapon fallback poll. The method event alone grants
a bounded 500 ms scene-acquisition window for transient Unity state. Closing
scope is still detected by the direct `m_IsZooming` read on the next active tick
(about 8 ms worst case), then the worker sleeps again.
It builds separately with `build_zygisk.ps1 -HookVersion` as
`output/rt_shim_hook_version.zip`; the normal build remains patch-free.

### v2.8.2-hook-lead-curve

Tuning chỉ áp dụng cho artifact hook. Maximum horizontal prediction horizon
tăng từ 90 lên 120 ms để đón xa hơn; cách đo root velocity, EMA 0.30, speed
weight liên tục và giới hạn 12 m/s giữ nguyên. Tốc độ correction giảm nhẹ:
`Kp=7.0`, cap mỗi trục từ `0.13 rad/s` ở 64 px trở xuống tới `0.23 rad/s` ở
300 px trở lên. Phần giữa dùng smoothstep thay vì linear để tâm vẫn đi nhanh
khi xa nhưng giảm đều khi tiến gần, không có góc đổi tốc độ tại hai ngưỡng.
Tại thời điểm thử nghiệm, bản poll vẫn giữ 90 ms, `Kp=7.5` và cap linear
0.15–0.25 rad/s.

### v2.8.2-poll-lead-curve

Theo kết quả trải nghiệm, nhánh hook bị loại bỏ khỏi source, build script và
artifact; ghi chú hook ở trên chỉ được giữ làm lịch sử. Runtime quay lại lịch
poll adaptive `5000/500/8 ms`, không sửa executable code page.

Phần tuning đã thử trên hook được chuyển sang bản poll: maximum horizontal
prediction horizon 120 ms, `Kp=7.0`, cap mỗi trục dùng smoothstep từ
`0.13 rad/s` tại 64 px trở xuống tới `0.23 rad/s` tại 300 px trở lên. Mục tiêu
là đón target chạy xa hơn nhưng giảm tốc camera đều khi tâm tiến gần. Version
code 284.

Build release đạt **70/70 test ARM64**. ZIP SHA-256
`595D9107CF46E14603DAE24DD5E83B808A14164B83291C0ACD1B0558B499DCCD`;
payload `3b46c5a24ae52c96d618f7be342812b87e122245303945d5ca69e25bdba27f19`
và loader `e25ae04142320432dc9570a31fba5353db3001f9ee83c32d4e3ef7861aacf8fe`.
Bản đã cài và reboot trên `192.168.5.102:5555`; game PID 6434 ổn định,
Unity/IL2CPP được nạp và không có fatal log. Callsite hook cũ tại IL2CPP RVA
`0x889b64c` đã trở lại instruction gốc `0x9400000d`
(`BL WNWeaponSniper.T_OnZooming`), xác nhận bản poll không redirect method.

### v2.8.3-poll-linear-lead

Live probe của v2.8.2 ghi nhận trong 20 giây có 1573 lần prediction và 1558
lệnh gyro active. Tốc độ target thường khoảng 1.4–2.5 m/s, tối đa 3.347 m/s;
do horizon cũ còn nhân `speed / 12`, phần lớn lần chạy chỉ thực sự dùng 14–25
ms dù giới hạn danh nghĩa là 120 ms. Lead lớn nhất quan sát được là 56.88 px.
Probe cũng thấy predictor thường quay về sample đầu sau khi trigger chớp false
rồi true trên cùng target.

Logic mới dùng horizon cố định 60 ms khi filtered speed đạt 0.80 m/s, vì vậy
khoảng lead tăng tuyến tính theo velocity. Chỉ vùng nhiễu thấp dùng smoothstep:
0 tại 0.25 m/s trở xuống và đạt đầy đủ tại 0.80 m/s; raw velocity vẫn bị giới
hạn an toàn ở 12 m/s. Khi cùng target được reacquire trong grace 250 ms, cycle
mới giữ lại predictor/EMA thay vì bắt đầu velocity từ zero. Version code 285.

Build release đạt **71/71 test ARM64**. ZIP SHA-256
`6D4C0C262C05D6BA679D68AC891400E3B2E358FAB0B09D0CA9AFAA0448FDE0E1`;
payload `bfbc92040d0aa4dfd44f9ff407967abd5cded4699bd2dcf9be9a9f15d27665ae`.
Bản đã cài và reboot trên `192.168.5.102:5555`; module active đúng versionCode
285, game PID 5547 ổn định, không có named module map hoặc fatal log.

### v2.8.4-poll-linear-lead (game update 2026-09-22)

Game CrossFire Legends cập nhật lên version `1.0.19.90` (versionCode 49).
Module được cập nhật lại theo profile mới:
- Unity build ID: `1304f8f523fbba8d98ab8d775ae4e3d696efdb04` (cũ: `1a60ff52...`)
- IL2CPP build ID: `33384ad3538f357d057987fa88c9f4eba485e563` (cũ: `a8793b51...`)
- RVA cờ registration readiness: `0xc698140` (cũ: `0xbe000e0`), xác minh trực tiếp trên game qua Frida đọc giá trị = 1
- RVA `Pawn.get_CurrentWeapon()`: `0x416ae50` (cũ: `0x6876c98`), giải mã trực tiếp từ con trỏ MethodInfo trong runtime
- Toàn bộ 10 class và 10 key field (`m_CachedUpperBodyTransform`, `m_DoingAimAssist`, `m_IsZooming`, `m_WorldCamera`...) đã được đối chiếu qua live probe — 100% nguyên vẹn.
- Version code 286.

Build release đạt **71/71 test ARM64**. ZIP SHA-256
`0493FB872C3D6DB003D93AC4EBFA17C1B80CC809E7E7610D1626B7C8CABDC8D4`;
payload `da507e2e6a4428d36ba6e6519877ce27e805a557287aec9e836a8481339e8c58`.

### v2.8.5-spectator-guard

Live read-only sampling captured the complete round transition. While the
local player was spectating, `PlayerController.m_IsSpectating = 1`,
`PlayerInfo.m_IsSpectating = 1`, `m_LocalPlayerRespawnState = 2`, the local
pawn health sentinel was `-4`, and `m_ViewTarget` pointed to the observed pawn.
At the next live round both spectator flags became zero, respawn state became
1, and health returned to 100. The cached scene now resolves and reads the
controller spectator flag before weapon classification. A true flag clears the
cached sniper and returns an inactive snapshot on the 500 ms cadence, causing
the gyro controller to reset immediately while still detecting the next round.

Release verification: **72/72 ARM64 tests passed** and `git diff --check`
reported no errors. ZIP SHA-256:
`98BBAE45415BF0F85FAA9A380D521104D436BA9EACA89ED3EC6E52AF93D98E4E`;
packaged payload SHA-256:
`968D5D94B66E61787A8D3FD038F5A8DAAC0FB79A537B643771A2896912F189`.

Installed on `192.168.5.102:5555` through KernelSU and rebooted. KernelSU
reported the enabled module as `2.8.5-spectator-guard` / `versionCode=287`.
The device-side payload SHA-256 matched the packaged payload, and the game
started normally after reboot without a new fatal signal.
