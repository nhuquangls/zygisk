> Version 2.2.0 connects native snapshots and companion input. See [the 200px build](zygisk-200px.md). The notes below preserve earlier analysis and tests.

# IL2CPP runtime: kết quả kiểm chứng ngày 2026-09-10

Kết quả mới hơn về tọa độ native Transform, camera và thử vuốt trong phòng tập:
[aim-live-test.md](aim-live-test.md). Các quan sát PID `3269` dưới đây được giữ
làm bằng chứng của lần kiểm tra resolver ban đầu.

Thiết bị ADB `192.168.5.102:5555`, Lenovo TB375FC, Android 16, ARM64.
Package `com.vnggames.cfl.crossfirelegends`, PID lúc kiểm tra `3269`.
Build ID `libil2cpp.so`: `a8793b51fee671e98de0cc0ad42bb85ffd5d0677`.

## Resolver đã hoạt động

Runtime có 24 assembly và export `il2cpp_*` theo tên. Native payload đọc bảng
ELF trong RAM vì thử `dlopen("libil2cpp.so", RTLD_NOLOAD)` không lấy được handle
ở ngữ cảnh nạp helper. Không sửa linker, không nạp bản IL2CPP khác. Resolver
chỉ chấp nhận named function symbol nằm trong đoạn PT_LOAD có quyền thực thi.

Tra `Assembly-CSharp.dll` → class theo namespace/tên → field theo tên. Kiểm tra
type kind, flags static/literal, offset và kích thước instance trước khi dùng.
Gắn thread vào IL2CPP khi cần và chỉ detach thread do chính lần gọi đó gắn.
Không gọi getter singleton, `runtime_class_init`, game method hay setter.
Các API metadata có thể khởi tạo cache IL2CPP, nên đây không phải phép đo hoàn
toàn không làm thay đổi trạng thái runtime.

Native resolver được gọi đồng bộ từ payload build thật nạp tạm vào tiến trình;
đã resolve đủ 22 field và manager/list. Không khởi chạy worker nền trong phép
thử này. Kết quả và SHA-256 payload: [native-validation-3269.json](../output/runtime_metadata/native-validation-3269.json).
ZIP chưa được cài; đường loader Zygisk khi khởi động tiến trình chưa được thử.

## Field offset quan trọng

Các số dưới đây là kết quả của build đã kiểm tra, không phải hằng số để dùng
cho mọi phiên bản. Native resolver lấy lại offset theo tên mỗi lần chạy.

| Class.field | Offset | Trạng thái quan sát |
|---|---:|---|
| AttackableTargetsManager.m_AttackableTargetList | `0x10` | ListWN có 10 pawn trong trận |
| AttackableTarget.m_Health | `0x70` | Có giá trị và thay đổi giữa các lần đọc |
| AttackableTarget.m_IsHidden | `0x7c` | Không đủ để kết luận line of sight |
| BaseObject.m_Transform | `0x18` | Transform sống trên 10 pawn |
| BaseComponent.m_Root / m_Game | `0x48` / `0x50` | ActorRoot / TeamGame sống |
| Pawn.m_PlayerInfo | `0x418` | PVPPlayerInfo sống |
| Pawn.m_Controller | `0x458` | Có trên local pawn trong mẫu |
| Controller.m_Pawn | `0x68` | Trỏ ngược đúng local pawn |
| PlayerController.m_LocalPlayer | `0x80` | LocalPlayer sống |
| LocalPlayer.m_PlayerController | `0x28` | Trỏ ngược đúng controller |
| LocalPlayer.m_Camp | `0x20` | Phe local là 1 trong mẫu |
| PlayerInfo.m_Camp | `0x128` | Phe 1/2, xác minh bằng tool chẩn đoán |
| UnityEngine.Object.m_CachedPtr | `0x10` | Native Transform, tool chẩn đoán |
| Pawn.HeadCharacterPosition / FootCharacterPosition | `0x824` / `0x830` | Đều `(0,0,0)` trong mẫu |
| Pawn.LastPlayerPosition | `0x258` | `(0,0,0)` trong mẫu |
| Pawn.m_HeadTransform / m_FirstPersonCamera | `0x428` / `0x6e8` | Null trong mẫu |
| Pawn.m_LocalPlayerController | `0x758` | Null; đường m_Controller hoạt động |

`PlayerInfo.m_Camp` và `Object.m_CachedPtr` mới được đọc trong script chẩn đoán,
chưa nằm trong bộ 22 field của payload. Offset của toàn bộ field/class đã lấy
nằm trong [metadata-match-3269-verified.json](../output/runtime_metadata/metadata-match-3269-verified.json).

## Snapshot trong trận

`AttackableTargetsManager._Instance` → `m_AttackableTargetList` có kiểu thực tế
`UnityTool.ListWN<WNEngine.AttackableTarget>`, kế thừa `List<T>`.
Các field container được lấy từ metadata của class thực tế:

| Field | Offset | Giá trị trong mẫu |
|---|---:|---:|
| `_items` | `0x10` | Mảng AttackableTarget[] |
| `_size` | `0x18` | 10 |
| `_version` | `0x1c` | Kiểm tra lại sau đọc |

Array length API trả capacity 16, element size 8. Script dùng header array
ARM64 32 byte, kiểm tra class từng phần tử assignable từ Pawn, rồi đọc field.
Cả 10 có class `WNPVPGame.PVPPlayerPawn`; có 5 pawn phe 1 và 5 pawn phe 2.
Mẫu slot 0 có chuỗi local pawn/controller/player khớp hai chiều, cùng camp 1.
Version, count và items không đổi trong lần lấy mẫu. Kiểm tra này chỉ xác nhận
container ổn định, không đảm bảo mọi field của mọi object thuộc cùng một frame.

Các cached position và camera null/zero được giữ nguyên trong báo cáo, không
thay bằng tọa độ suy đoán. Cần xác minh layout native Transform, nguồn head,
camera/view-projection và vòng đời dữ liệu trước khi nối snapshot vào aim.
Chưa có adapter input Android hoặc vòng điều chỉnh aim hoạt động.

## Công cụ tái hiện

Frida 17.17.0 được dùng để đối chiếu trong phiên debug; module không phụ thuộc
Frida. Dùng server/client cùng phiên bản, ADB forward tới server đang chạy,
và PID hiện tại của game. Script không tự khởi động server hoặc game.

```powershell
python tools/runtime_metadata.py --pid <PID> --inspect-roots --output output/runtime_metadata/metadata-new.json
```

Để thử native, chép đúng `build/readonly_native/libgcloudsync.so` vào đường
dẫn app có thể nạp, đối chiếu SHA-256 trên thiết bị, rồi:

```powershell
python tools/validate_runtime_resolver.py --pid <PID> --device-library <path-on-device> --metadata output/runtime_metadata/metadata-new.json --output output/runtime_metadata/native-new.json
```

Tool dùng symbol của chính bản build để gọi resolver, nhả dlopen handle trong
`finally`, rồi detach phiên Frida. Không gọi `cf_payload_start` hay cài ZIP.
Các địa chỉ tuyệt đối trong JSON chỉ có giá trị cho PID/phiên đã ghi.

Tham chiếu định dạng: [ELF Dynamic Linking](https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.dynamic.html),
[Frida JavaScript API](https://frida.re/docs/javascript-api/).
