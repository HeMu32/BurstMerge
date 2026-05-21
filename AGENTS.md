Reference\hdr-plus-swift 是一个macOS项目, 包括源代码, 我们现在想将他的核心能力移植到GNU工具链, gcc等. 
核心功能意味着我们的关注是除了前端能力之外的部分. 

Reference\文件夹中有一些参考资料. 当你需要寻求参考的时候, 优先检查这个文件夹中的内容. 
里面包括: 
原始论文 hdrplus-paper
参考实现项目 hdr-plus-swift
开源RAW解码器 LibRAW (不允许使用这个项目的源代码, 项目仅仅用于参考对于raw文件的数据解释, 避免瞎猜, 不能用这个项目的代码来进行RAW文件处理)
DNG格式参考 DNG_Spec_1_7_1_0

项目采用MinGW / G++编译. 不允许使用MSVC编译. 
目前项目只关注Windows平台适配性, 其余平台的支持暂时作为后续拓展考虑. 

项目目录结构说明:
	- CMakeLists.txt: 顶层 CMake 构建入口。
	- apps/: 可执行程序目录，包含 cli/、console/ 等应用目标。
	- libburstmerge/: 核心库源码和单元测试目录，包括 include/、src/、test/。
	- 3rdparty/: 第三方依赖目录，例如 dng_sdk、openmp、pocketfft、vulkan 等。
	- Reference/: 参考资料目录，包含 hdr-plus-swift、hdrplus-paper、LibRaw、DNG 规范等。
	- docs/: 项目文档与计划文件。
	- build/: CMake 生成的构建输出目录。

