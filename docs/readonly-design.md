> Version 2.2.0 connects native snapshots and companion input. See [the 200px build](zygisk-200px.md). The notes below preserve earlier analysis and tests.

# Thiết kế gọn: đọc dữ liệu → tính aim → input Android

Yêu cầu: Zygisk nạp module vào tiến trình mục tiêu; đọc enemy/camera;
tính hướng điều chỉnh; đưa thao tác vào luồng input Android. Không cần ImGui,
overlay, hook EGL, patch game, patch ACE, Shadow Memory hay lọc `/proc/self/maps`.

## Phần đã triển khai

- `native/readonly_memory.c`: đọc bộ nhớ của chính tiến trình bằng
  `process_vm_readv`. Chặn tràn địa chỉ và chuỗi con trỏ quá dài; nếu kernel trả
  lỗi hoặc chỉ đọc được một phần thì xóa kết quả, trả thất bại. Không đổi quyền
  trang hay cài signal handler để đọc tiếp.
- `native/readonly_module.c`: tìm thư viện đã nạp bằng `dl_iterate_phdr`, lấy
  load bias, khoảng PT_LOAD và GNU build ID nếu đọc được.
- `native/aim_math.c`: phép chiếu với view-projection matrix row-major, lọc địch
  sống/nhìn thấy/không bất tử, chọn mục tiêu gần tâm trong FOV và tính delta theo
  smooth. Từ chối frame cũ, giá trị NaN/Inf và tọa độ ngoài màn hình. Đây là toán
  trên snapshot do caller cung cấp; chưa có adapter lấy snapshot từ game.
- `native/readonly_exports.c`: đọc bảng export ELF đang nạp, hỗ trợ SysV/GNU
  hash, kiểm tra tên chính xác, loại symbol và đoạn mã thực thi. Không cần handle
  `dlopen` của game, không scan mẫu lệnh hay dùng RVA cố định.
- `native/il2cpp_metadata.c`: tra 22 field theo tên, kiểm tra type/flags và giới
  hạn instance; đọc singleton manager/list hiện có. Metadata query và attach
  thread có thể cập nhật cache runtime; không gọi getter/gameplay/setter.
- `native/readonly_runtime.c`: worker đợi thư viện/metadata tối đa 120 lần mỗi
  bước, resolve rồi báo trạng thái và kết thúc. Nó chưa chạy vòng aim hoặc gửi input.
- Build dùng C và NDK, không còn dependency xDL/Dobby hoặc assembly bridge.
  Linker loại các helper chưa được runtime sử dụng; test biên dịch lại cùng mã
  nguồn để kiểm tra riêng. File legacy không được biên dịch hay đóng gói.

Đây là bản metadata/probe, **chưa phải module aim hoạt động**. Đã gọi resolver
native trực tiếp trên thiết bị bằng thư viện nạp tạm; chưa cài ZIP. Module ID giữ nguyên để khi cài bản
mới, nó thay thế bản patch cũ thay vì chạy song song. Cần khởi động lại tiến
trình để loại các patch của bản cũ đã nằm trong RAM.

## Pipeline cần nối sau khi có dữ liệu hợp lệ

```text
Worker theo nhịp cố định
    └─ đọc snapshot enemy + camera + trạng thái trận
         └─ kiểm tra tuổi snapshot / tọa độ / điều kiện mục tiêu
              └─ chiếu màn hình → FOV → smooth
                   └─ đổi sai lệch ngắm thành độ vuốt theo sensitivity
                        └─ gửi MotionEvent vào input của ứng dụng
```

Không cần hook render để có nhịp cập nhật. Khi nối worker liên tục, dùng thời
gian monotonic và lịch tuyệt đối để tránh trôi nhịp; không coi `.05` mỗi tick là
tốc độ tương đương ở mọi tần số. Điểm màn hình chỉ là sai lệch ngắm, chưa phải
độ vuốt đúng: cần hiệu chuẩn sensitivity, chiều trục, orientation và viewport.

Input cần quản lý pointer ID, DOWN/MOVE/UP/CANCEL, focus/pause và thao tác thật
của người chơi. Không gửi các cặp DOWN/UP độc lập ở mỗi frame hoặc chèn một
MotionEvent đơn điểm vào giữa chuỗi đa điểm đang diễn ra. Khi dữ liệu mất hoặc
ra khỏi trận, dừng điều chỉnh và giải phóng gesture do module sở hữu.

## Những gì tham khảo được từ analysis-part2.md

- Tách phần đọc bộ nhớ, phép toán và input là hướng phù hợp với yêu cầu.
- WorldToScreen, khoảng cách tới tâm, FOV và smooth là công thức có thể dùng
  trên snapshot đã xác minh. Test dùng FOV 200 px, smooth .05; runtime chưa có
  cấu hình aim đang hoạt động.
- Dữ liệu trong bài không đủ để coi `RVA_BATTLE_MANAGER` hoặc các offset
  `0x38`, `0x18`, `0x90` là layout đã kiểm chứng cho bản game này.
- `ANativeWindow_getHeight` chỉ trả chiều cao surface; hook nó không tự tạo
  kênh gửi touch. [Android NDK](https://developer.android.com/ndk/reference/group/a-native-window).
- `onTouch`/`MotionEventClick` được trích chỉ chứng minh nhận cảm ứng cho View;
  chưa chứng minh gửi sự kiện vào game.
  [Android OnTouchListener](https://developer.android.com/reference/android/view/View.OnTouchListener).
- Không suy ra rằng chỉ đọc bộ nhớ hoặc dùng Zygisk đồng nghĩa không thể bị
  phát hiện. Thiết kế này không triển khai chức năng đối phó ACE.

## Dữ liệu hiện có và phần còn thiếu

`zygisk/dump.cs` và `ghidra/dump.cs` giống nhau theo SHA-256:
`A997F08FA4583563035FBE5FF572B7003B3915F4146BA4ACBF45A06D8A6C7E4F`.

File có `WNEngine.AttackableTargetsManager` (dòng 229240), `WNEngine.Pawn`
(229588), `WNEngine.PlayerController` (234376), nhưng field/method bị giải mã
sai, không có field offset hay method RVA. Ví dụ field tên rỗng/`mscorlib`,
method có số lượng tham số 65535. TypeDefIndex/FieldDefIndex không phải địa chỉ
hay offset có thể dùng để đọc object. Không tìm thấy `BattleManager` hoặc
`m_CurrentAimAssistTarget` trong file này.

Đã xác nhận từ Smali APK gốc trên máy:

- `reverse/orig_decoded/smali_classes5/com/unity3d/player/UnityPlayer.smali`
  có `currentActivity`, `currentPlayer` và `injectEvent(InputEvent): boolean`.
- Activity của game gọi `UnityPlayer.injectEvent` trong đường xử lý input.

Đây là điểm để kiểm chứng adapter input, không phải bằng chứng rằng có thể gọi
tùy ý từ worker hoặc ghép cảm ứng ảo mà không ảnh hưởng cảm ứng thật. Chưa viết
adapter JNI hay gửi sự kiện thử. Quy tắc pointer/index cần tuân theo
[MotionEvent](https://developer.android.com/reference/android/view/MotionEvent).

Kết quả runtime mới nằm trong [runtime-metadata.md](runtime-metadata.md): đã
có offset metadata, singleton/list, 10 pawn, camp và chuỗi nhận diện local pawn.
Các field head position/camera đã thử vẫn 0/null; `m_Transform` có con trỏ sống.
Metadata hợp lệ không chứng minh một field đang được game sử dụng để cập nhật
tọa độ. Cờ `m_IsHidden` cũng không thay thế phép kiểm tra che khuất.

Còn cần dữ liệu runtime có thể kiểm chứng:

1. Adapter snapshot từ container đã xác minh, nguồn tọa độ head/transform thực
   tế, vòng đời object và điều kiện alive/visible/invulnerable của chế độ chơi.
2. Gốc camera và cách đọc view-projection matrix, quy ước layout/trục; trạng thái
   trận và vòng đời object để không dùng con trỏ từ trận trước.
3. Kiểm tra luồng input trên thiết bị, cách đồng bộ với touch thật và hệ số đổi
   sai lệch màn hình sang độ vuốt. Xác minh build ID trước khi dùng layout.

Không điền offset phỏng đoán từ mã minh họa vào bản build.
