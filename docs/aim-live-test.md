> Version 2.2.0 connects native snapshots and companion input. See [the 200px build](zygisk-200px.md). The notes below preserve earlier analysis and tests.

# Thử snapshot và input Android trong phòng tập

Phiên kiểm tra tiếp theo dùng PID `7162`, `libil2cpp.so` base `0x71a9818000`.
PID cũ `3269` đã kết thúc; metadata và con trỏ được lấy lại cho PID mới.
Đây là công cụ thử từ máy tính qua Frida/ADB, chưa phải vòng aim trong ZIP Zygisk.

**Kết quả mới nhất:** sau khi cả aim thường và aim sniper đều tắt, phép thử đã
đưa tâm về điểm thân trên của một bot đứng yên: sai lệch từ 138.72 px xuống
7.996 px. Lần đọc sau khi dừng vuốt vẫn ghi đúng mục tiêu và cùng sai lệch.
Chưa kiểm tra bám bot chuyển động, head bone, nhiều mức zoom hoặc input của module.

## Tọa độ và camera đã đọc được

Các field `HeadCharacterPosition`, `m_HeadTransform` và camera trên Pawn vẫn
0/null. Đường đọc có dữ liệu trong phòng tập:

- `BaseObject.m_Transform` → `UnityEngine.Object.m_CachedPtr` → native Transform.
- `Pawn.m_CachedUpperBodyTransform` cung cấp điểm thân trên. Không coi đây là
  head bone đã xác minh hoặc điểm bảo đảm headshot.
- Static `WNEngine.PlayerCamera.m_WorldCamera` cung cấp Unity Camera đang dùng.
- `PlayerCamera` còn có cached position, forward và `LastWorldCameraUpdateFrame`.

Đã đọc mã getter Unity để xác định layout; không gọi các getter đó:

| Vị trí | Layout trên libunity đã kiểm tra |
|---|---|
| Native Transform | `+0x40` trỏ hierarchy, `+0x48` chứa index |
| Hierarchy | `+0x08` trỏ mảng TRS, `+0x10` trỏ parent indices |
| Một record TRS, stride 40 byte | position 3 float, quaternion 4 float, scale 3 float |
| Native Camera | view matrix `+0x48`, projection matrix `+0x88` |

Tọa độ world được tính bằng chuỗi parent TRS; chặn index, chu trình, độ sâu,
NaN/Inf và quaternion không chuẩn hóa. Các offset native này chỉ là profile
của binary đã kiểm tra. Script chặn nếu build ID libunity khác
`1a60ff52f7bb4ad5de0465b12a83aba3d7af0700`; không dùng các offset này cho build khác.
Mã disassembly lưu tại [unity-accessors-leaf-7162.json](../output/runtime_metadata/unity-accessors-leaf-7162.json)
và [transform-hierarchy-code-7162.json](../output/runtime_metadata/transform-hierarchy-code-7162.json).

Phép chiếu column-major `projection * view * world` khớp vị trí bot trên ảnh
2944×1840. Camera socket của local pawn khớp cached camera position. Projection
matrix cho FOV dọc khoảng 20° khi đang ngắm sniper; cached scalar
`m_WorldCameraFOV` vẫn là 51°, nên không dùng scalar đó thay projection matrix.

`aim_snapshot.js` đọc lại singleton/list mỗi lần, kiểm tra class, camp, health,
hidden/visible và local pawn; kiểm tra frame/list cùng ma trận trước/sau khi đọc.
Ma trận view phải khớp cached camera pose. Cờ dirty native không được coi là
bảo đảm cache mới: kết quả chỉ là snapshot chẩn đoán đã đối chiếu trên màn hình.
`m_IsPawnVisible` chưa được xác nhận là kiểm tra che khuất bằng raycast.

## Thử input và giới hạn

`aim_input_probe.py` mặc định chỉ preview. Với `--test-input`, nó khóa một bot
trong FOV 200 px, gửi tối đa hai vuốt hiệu chuẩn và bốn vuốt điều chỉnh, mỗi
trục tối đa 40 px. Vùng bắt đầu vuốt được chọn theo HUD hiện tại trong phòng
tập; chưa tự tìm vùng input phù hợp cho layout khác. Tool không bấm bắn.

Input đi qua `adb shell input touchscreen swipe`. Đây chưa phải adapter input
chạy trong module; chưa giải quyết ghép gesture với cảm ứng thật của người chơi.

Các lần thử đầu chưa hội tụ. Sau khi người dùng tắt aim thường, runtime ghi
`m_EnableAimAssistance=0`, `m_EnableAimAssistanceForSniper=1`. Mã
`ShouldAdjustRotationByAimAssistance` có nhánh đọc riêng hai field này, nên
không suy ra tắt aim thường đã tắt nhánh sniper. Bước hiệu chuẩn ngang có đáp
ứng quá yếu so với bước dọc; tool hiện từ chối phép hiệu chuẩn có độ chênh quá
lớn giữa hai trục và cập nhật dần độ nhạy từ phản hồi sau mỗi bước.

Báo cáo thử:

- [Lần đầu](../output/runtime_metadata/aim-input-test-7162.json).
- [Sau khi tắt aim thường](../output/runtime_metadata/aim-input-retry-7162.json).
- [Hiệu chuẩn 32 px, đã dừng vì phản hồi không nhất quán](../output/runtime_metadata/aim-input-calibrated-7162.json).

## Lần thử sau khi tắt cả hai loại aim của game

Runtime xác nhận `enabled=0`, `sniperEnabled=0`, `doing=0`. Ở mức zoom FOV dọc
20°, tool dùng hai vuốt 32 px để hiệu chuẩn, rồi ba vuốt điều chỉnh
`(-40,-22)`, `(-35,-19)`, `(-11,-6)`. Sai lệch cuối theo trục là
`(-7.452,-2.898)` px, độ lớn 7.996 px; ban đầu là 138.72 px.

- [Báo cáo vuốt và phản hồi](../output/runtime_metadata/aim-both-off-test-7162.json).
- [Đọc lại sau khi dừng input](../output/runtime_metadata/aim-both-off-hold-7162.json).
- [Ảnh màn hình sau thử](../output/runtime_metadata/aim-both-off-screen-7162.png).

Kết quả xác nhận chuỗi đọc dữ liệu → chiếu → điều chỉnh qua input Android đã
hoạt động trong ca thử bot đứng yên này. Mục tiêu là điểm thân trên lấy từ
Transform, không phải bảo đảm headshot. ZIP hiện tại vẫn là metadata probe;
cần đưa snapshot/input vào native worker và xử lý vòng đời cùng cảm ứng thật
trước khi có module aim độc lập.
