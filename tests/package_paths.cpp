#include "Package.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const auto package = PackageDirectory(argv[0]);
    const auto launcher = LauncherDirectory(argv[0]);
    std::cout << "Package: " << package << "\nLauncher: " << launcher << '\n';
    return std::filesystem::equivalent(package, argv[1]) &&
        std::filesystem::equivalent(launcher, argv[2]) ? 0 : 1;
}
