# Đánh giá phương án external daemon

Phương án tách chương trình thành tiến trình root riêng có thể đưa bộ đọc dữ liệu và phần tính aim ra khỏi không gian địa chỉ của game. Tuy nhiên, với code hiện tại, đây là thay đổi kiến trúc; không chỉ đổi `getpid()` thành PID game.

## Phần tái sử dụng và phần phải đổi

| Thành phần | Đánh giá theo code hiện tại |
|---|---|
| `aim_math.c`, `aim_policy.c` | Có thể giữ phần lớn: phép chiếu, chọn mục tiêu, khóa mục tiêu, giới hạn 200 px và kiểm tra độ mới của snapshot. |
| Các struct snapshot/vector/matrix | Có thể giữ hợp đồng dữ liệu. Địa chỉ trong game phải được coi là địa chỉ từ xa, không được dereference trong daemon. |
| `readonly_memory.c` | Cần ngữ cảnh tiến trình đích, xử lý tiến trình thoát/PID được dùng lại, đọc thiếu và dữ liệu thay đổi giữa các lượt đọc. |
| `readonly_module.c` | Hiện dùng `dl_iterate_phdr` trong tiến trình. Bản external cần cơ chế nhận diện module và load bias của tiến trình đích. |
| `il2cpp_metadata.c` | Không thể giữ nguyên: hiện gọi `domain_get`, `thread_attach`, `class_from_name`, các getter metadata và `field_static_get_value` bằng con trỏ hàm cục bộ. |
| `scene_snapshot.c` | Giữ được phần tính toán và kiểm tra snapshot; phải thay phần khởi tạo metadata, nhận diện class và đọc static field đang gọi API IL2CPP. |
| `android_input.c` / DEX | Hiện dựa vào JavaVM, Activity và Window.Callback của game để biết focus, viewport và trạng thái chạm. Daemon phải có nguồn thông tin thay thế. |
| Zygisk loader / root companion | Có thể bỏ trong kiến trúc external hoàn chỉnh, nhưng cần vòng đời daemon, khởi động/dừng và quản lý kết nối với từng phiên game. |

Chưa có cơ sở để khẳng định tái sử dụng “90%”. Phần toán học độc lập tốt; phần lấy dữ liệu và input phụ thuộc nhiều vào việc chạy trong game.

## Những điểm cần diễn đạt chính xác

- **Đọc bộ nhớ khác tiến trình:** `process_vm_readv` chịu kiểm tra quyền truy cập và có thể đọc thiếu; nó cũng không bảo đảm snapshot nguyên tử. Quyền root không thay thế việc kiểm chứng đường đọc trên thiết bị. [Linux man-pages](https://man7.org/linux/man-pages/man2/process_vm_readv.2.html).
- **Input qua event node:** đây vẫn là input được phần mềm tạo ra. Cần đúng thiết bị, miền tọa độ, hướng màn hình và trạng thái multi-touch; ghi một cặp tọa độ không đủ để xử lý an toàn khi người dùng đang giữ joystick hoặc nút bắn. [Linux multi-touch protocol](https://docs.kernel.org/input/multi-touch-protocol.html), [Android touch devices](https://source.android.com/docs/core/interaction/input/touch-devices).
- **Không nạp thư viện vào game:** có thể kiểm tra được thuộc tính cụ thể này sau khi bỏ đường Zygisk hiện tại. Nó không chứng minh toàn bộ hệ thống “sạch 100%” hay không thể bị phát hiện. Daemon, file thực thi, quyền root và hành vi input vẫn là các phần riêng của hệ thống.
- **Read-only hiện tại:** không patch trường game hoặc mã game, nhưng resolver vẫn gọi vào runtime, bao gồm đăng ký thread. Vì thế nó chưa phải bộ đọc từ xa chỉ dùng syscall.

## Đề xuất triển khai nếu chọn chuyển kiến trúc

Giữ lõi toán học và hợp đồng snapshot. Trước hết kiểm chứng một bộ đọc external chỉ xuất snapshot trên bản game đang dùng, bao gồm lúc mở game, vào trận, đổi scene và game khởi động lại. Sau khi nguồn dữ liệu ổn định mới nối bộ điều khiển input và kiểm chứng lại giới hạn 200 px, trạng thái chạm và nhả chạm. Đây là đề xuất thiết kế; chưa chuyển code hoặc cài daemon trong lần đánh giá này.
