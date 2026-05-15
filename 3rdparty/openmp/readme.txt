MinGW (GCC 11.2.0) 在此环境中将 OpenMP 运行时静态链接 (libgomp.a)。
可执行文件不依赖 libgomp-1.dll，仅依赖 KERNEL32.dll + msvcrt.dll。
若迁移到 MSVC 工具链或需要动态链接的 MinGW 构建，需将 libgomp-1.dll
拷贝到此目录用于分发打包。
