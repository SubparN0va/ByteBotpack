#include "Package.h"
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const auto expected = std::string_view(argv[1]) == "megabyte"
        ? BotProfile::Megabyte : BotProfile::KilobyteReduced;
    if (CompiledBotProfile() != expected) {
        std::cerr << "Compiled bot profile does not match the build selection\n";
        return 1;
    }
    return 0;
}
