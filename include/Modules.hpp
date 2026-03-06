#pragma once

#include <string>
#include <set>

namespace nexa {

// Tracks which standard library modules are enabled by #include directives
class Modules {
public:
    void enable(const std::string& path) {
        enabled_.insert(path);
    }

    bool hasIo() const {
        return enabled_.count("std/io") > 0;
    }

    bool hasOs() const {
        return enabled_.count("std/os") > 0;
    }

    bool hasDll() const {
        return enabled_.count("std/dll") > 0;
    }

    bool hasFile() const {
        return enabled_.count("std/file") > 0;
    }

    bool hasRandom() const {
        return enabled_.count("std/random") > 0;
    }

    bool hasTime() const {
        return enabled_.count("std/time") > 0;
    }

    std::string getCppIncludes() const {
        std::string out;
        if (hasIo()) {
            out += "#include <cstdio>\n";
            out += "#include <cstring>\n";
            out += "#include <cstdlib>\n";
            out += "#include <string>\n";
            out += "static int __nexa_to_int(const std::string& s) { try { return std::stoi(s); } catch(...) { return 0; } }\n";
        }
        if (hasOs()) {
            out += "#include <cstdlib>\n";
            out += "#include <string>\n";
            out += "#ifdef __linux__\n#include <unistd.h>\n#endif\n";
            out += "#ifdef _WIN32\n#include <windows.h>\n#endif\n";
            out += "#ifdef __APPLE__\n#include <mach-o/dyld.h>\n#endif\n";
            out += "static std::string __nexa_os_platform() {\n";
            out += "#ifdef _WIN32\n";
            out += "  return \"windows\";\n";
            out += "#elif defined(__APPLE__)\n";
            out += "  return \"darwin\";\n";
            out += "#elif defined(__linux__)\n";
            out += "  return \"linux\";\n";
            out += "#else\n";
            out += "  return \"unknown\";\n";
            out += "#endif\n";
            out += "}\n";
            out += "static std::string __nexa_exe_dir() {\n";
            out += "#ifdef __linux__\n";
            out += "  char buf[4096]; ssize_t n = readlink(\"/proc/self/exe\", buf, sizeof(buf)-1);\n";
            out += "  if (n > 0) { buf[n]=0; std::string s(buf); size_t p=s.find_last_of('/'); return p!=std::string::npos ? s.substr(0,p+1) : \"./\"; }\n";
            out += "#elif defined(_WIN32)\n";
            out += "  char buf[4096]; if (GetModuleFileNameA(NULL, buf, sizeof(buf))) {\n";
            out += "    std::string s(buf); size_t p=s.find_last_of(\"/\\\\\"); return p!=std::string::npos ? s.substr(0,p+1) : \".\\\\\"; }\n";
            out += "#elif defined(__APPLE__)\n";
            out += "  char buf[4096]; uint32_t sz=sizeof(buf); if (_NSGetExecutablePath(buf,&sz)==0) {\n";
            out += "    std::string s(buf); size_t p=s.find_last_of('/'); return p!=std::string::npos ? s.substr(0,p+1) : \"./\"; }\n";
            out += "#endif\n";
            out += "  return \"./\";\n";
            out += "}\n";
        }
        if (hasFile()) {
            out += "#include <fstream>\n";
            out += "#include <sstream>\n";
            out += "#include <filesystem>\n";
        }
        if (hasRandom()) {
            out += "#include <random>\n";
            out += "static std::mt19937& __nexa_rng() { static std::mt19937 gen(std::random_device{}()); return gen; }\n";
            out += "static void __nexa_random_seed(int s) { __nexa_rng().seed(static_cast<unsigned>(s)); }\n";
            out += "static int __nexa_random_int(int a, int b) { return std::uniform_int_distribution<int>(a, b)(__nexa_rng()); }\n";
        }
        if (hasTime()) {
            out += "#include <thread>\n";
            out += "#include <chrono>\n";
        }
        if (hasDll()) {
            out += "#include <vector>\n";
#ifdef _WIN32
            out += "#include <windows.h>\n";
#else
            out += "#include <dlfcn.h>\n";
#endif
            out += "static std::vector<void*> __nexa_dll_handles;\n";
        }
        return out;
    }

private:
    std::set<std::string> enabled_;
};

}  // namespace nexa
