# MultiPaste

一个轻量的 Windows 多段文本追加工具。核心建立在系统剪贴板之上，不依赖目标应用是否支持 UI Automation；只要应用能够正常复制文本，就可以参与追加。

## 使用方式

- `Ctrl+Alt+C`：默认的智能填充追加。短按后自动执行标准复制，并在需要时插入配置的填充符号。
- `Ctrl+Shift+C`：默认的无填充原样追加。
- 长按任一追加快捷键 400 毫秒：进入 5 秒“等待下一次真实复制”模式，可使用目标应用自己的快捷键、右键菜单或工具栏完成复制。
- `Ctrl+C`：保持普通复制行为。

成功操作完全静默。失败时显示无声音、不抢焦点的简短托盘通知；快捷键被其他程序占用时会明确提示冲突。

## 托盘与面板

右键单击任务栏通知区域中的 MultiPaste 图标，可选择：

- **面板**：自定义智能填充追加和无填充追加的快捷键、智能填充符号，以及是否开机自动启动。
- **退出**：安全注销快捷键并退出程序。

双击托盘图标也可以打开面板。Explorer 重启后，托盘图标会自动恢复。

智能填充符号支持普通文字，也支持以下转义：

- `\s`：空格（默认）
- `\n`：换行
- `\t`：制表符
- `\\`：反斜杠
- 空内容：不插入任何字符

面板配置保存在 `%LOCALAPPDATA%\MultiPaste\settings.ini`。开机自启动仅写入当前用户的 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，值中使用当前可执行文件的完整引用路径。

## 剪贴板历史

程序会尽力删除 `Win+V` 中本次复制产生的临时片段。只有当最新历史项与最终文本一致、紧邻项与临时片段一致且时间足够接近时才删除，避免误删已有记录。

## 隐私

- 不联网、不收集遥测。
- 不记录或持久化剪贴板内容。
- 临时文本仅存在于内存，事务完成或取消后主动擦除。
- 配置文件只保存快捷键和填充符号。

## 构建

需要 Visual Studio 2022 的“使用 C++ 的桌面开发”工作负载和 CMake 3.24 或更高版本。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

生成文件位于 `build/MultiPaste.exe`。

## 自动构建与发布

GitHub Actions 会在推送到 `main` 或创建 Pull Request 时自动执行 Windows x64 编译和测试。

发布新版本有两种方式：

1. 在 GitHub 仓库的 **Actions → Release → Run workflow** 中输入版本号，例如 `0.2.0`。
2. 在本地推送符合 `v*` 格式的标签：

```powershell
git tag v0.2.0
git push origin v0.2.0
```

发布成功后，GitHub Releases 会提供：

- `MultiPaste-<版本>-Setup-x64.exe`：当前用户安装包，不需要管理员权限。
- `MultiPaste-<版本>-win-x64.zip`：免安装便携版。
- `SHA256SUMS.txt`：下载文件的 SHA-256 校验值。

当前安装包尚未进行商业代码签名，因此 Windows SmartScreen 在下载量较少时可能显示未知发布者警告。
