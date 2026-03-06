#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <filesystem>
#include <cstdlib>

namespace nexa {
namespace pkg {

static std::string getHome() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
    return h ? h : ".";
#else
    const char* h = std::getenv("HOME");
    return h ? h : "/tmp";
#endif
}

static std::string getPackagesDir() {
    return getHome() + "/.nexa/packages";
}

static bool parseJsonString(const std::string& s, size_t& i, std::string& out) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;
    if (i >= s.size() || s[i] != '"') return false;
    i++;
    out.clear();
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') { i++; if (i < s.size()) out += s[i++]; }
        else out += s[i++];
    }
    if (i < s.size()) i++;
    return true;
}

static void parseNexapkgJson(const std::string& path, std::map<std::string, std::string>& deps) {
    std::ifstream f(path);
    if (!f) return;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    deps.clear();
    size_t i = content.find("\"dependencies\"");
    if (i == std::string::npos) return;
    i = content.find('{', i);
    if (i == std::string::npos) return;
    i++;
    while (i < content.size()) {
        while (i < content.size() && (content[i] == ' ' || content[i] == '\t' || content[i] == '\n' || content[i] == ':' || content[i] == ',')) i++;
        if (i >= content.size() || content[i] == '}') break;
        std::string key, val;
        if (!parseJsonString(content, i, key)) break;
        while (i < content.size() && (content[i] == ' ' || content[i] == ':' || content[i] == ',')) i++;
        if (!parseJsonString(content, i, val)) break;
        deps[key] = val;
    }
}

static int cmdInit(const std::string& dir) {
    namespace fs = std::filesystem;
    fs::path base = dir.empty() ? fs::current_path() : fs::path(dir);
    fs::path manifest = base / "nexapkg.json";
    if (fs::exists(manifest)) {
        std::cerr << "[nexapkg] nexapkg.json already exists.\n";
        return 1;
    }
    std::ofstream(manifest) << "{\n  \"name\": \"myapp\",\n  \"dependencies\": {}\n}\n";
    std::cout << "[nexapkg] Created nexapkg.json\n";
    return 0;
}

static int cmdAdd(const std::string& spec, const std::string& dir, const std::string& asPath = "") {
    namespace fs = std::filesystem;
    fs::path base = dir.empty() ? fs::current_path() : fs::path(dir);
    fs::path manifest = base / "nexapkg.json";
    if (!fs::exists(manifest)) {
        std::cerr << "[nexapkg] Run 'nexapkg init' first.\n";
        return 1;
    }
    std::string pkgName, pkgSrc;
    if (spec.find('/') == std::string::npos && spec.substr(0, 2) != "./" && spec.find("http") != 0) {
        fs::path globalPkg = fs::path(getPackagesDir()) / spec;
        if (fs::exists(globalPkg) && fs::is_directory(globalPkg)) {
            pkgName = spec;
            pkgSrc = "file:" + fs::absolute(globalPkg).string();
        } else {
            pkgName = spec;
            pkgSrc = "https://github.com/" + spec;
        }
    } else if (spec.size() >= 2 && spec.substr(0, 2) == "./") {
        fs::path localPath = (base / spec).lexically_normal();
        if (!fs::exists(localPath)) {
            std::cerr << "[nexapkg] Path not found: " << spec << "\n";
            return 1;
        }
        pkgName = localPath.filename().string();
        if (pkgName.size() > 4 && pkgName.substr(pkgName.size() - 4) == ".nxa") pkgName = pkgName.substr(0, pkgName.size() - 4);
        pkgSrc = "file:" + fs::absolute(localPath).string();
    } else {
        size_t slash = spec.rfind('/');
        pkgName = (slash != std::string::npos) ? spec.substr(slash + 1) : spec;
        if (pkgName.size() > 4 && pkgName.substr(pkgName.size() - 4) == ".nxa") pkgName = pkgName.substr(0, pkgName.size() - 4);
        pkgSrc = (spec.find("http") == 0) ? spec : "https://github.com/" + spec;
    }
    std::map<std::string, std::string> deps;
    parseNexapkgJson(manifest.string(), deps);
    std::string depKey = asPath.empty() ? pkgName : asPath;
    deps[depKey] = pkgSrc;
    std::ostringstream out;
    out << "{\n  \"name\": \"myapp\",\n  \"dependencies\": {\n";
    bool first = true;
    for (const auto& [k, v] : deps) {
        if (!first) out << ",\n";
        out << "    \"" << k << "\": \"" << v << "\"";
        first = false;
    }
    out << "\n  }\n}\n";
    std::ofstream(manifest) << out.str();
    std::cout << "[nexapkg] Added " << depKey << "\n";
    return 0;
}

static int cmdInstall(const std::string& dir, bool global, const std::string& singlePkg = "") {
    namespace fs = std::filesystem;
    std::map<std::string, std::string> deps;
    std::string pkgDir;
    if (!singlePkg.empty() && global) {
        pkgDir = getPackagesDir();
        std::string pkgName, pkgSrc;
        if (singlePkg.size() >= 2 && singlePkg.substr(0, 2) == "./") {
            fs::path localPath = fs::path(dir.empty() ? fs::current_path() : fs::path(dir)) / singlePkg;
            localPath = localPath.lexically_normal();
            if (!fs::exists(localPath)) {
                std::cerr << "[nexapkg] Path not found: " << singlePkg << "\n";
                return 1;
            }
            pkgName = localPath.filename().string();
            if (pkgName.size() > 4 && pkgName.substr(pkgName.size() - 4) == ".nxa") pkgName = pkgName.substr(0, pkgName.size() - 4);
            pkgSrc = "file:" + fs::absolute(localPath).string();
        } else {
            size_t slash = singlePkg.rfind('/');
            pkgName = (slash != std::string::npos) ? singlePkg.substr(slash + 1) : singlePkg;
            if (pkgName.size() > 4 && pkgName.substr(pkgName.size() - 4) == ".nxa") pkgName = pkgName.substr(0, pkgName.size() - 4);
            pkgSrc = (singlePkg.find("http") == 0) ? singlePkg : "https://github.com/" + singlePkg;
        }
        deps[pkgName] = pkgSrc;
    } else {
        fs::path base = dir.empty() ? fs::current_path() : fs::path(dir);
        fs::path manifest = base / "nexapkg.json";
        if (!fs::exists(manifest)) {
            std::cerr << "[nexapkg] nexapkg.json not found. Run 'nexapkg init' first.\n";
            return 1;
        }
        parseNexapkgJson(manifest.string(), deps);
        pkgDir = global ? getPackagesDir() : (base.string() + "/.nexa/packages");
    }
    fs::path projBase = dir.empty() ? fs::current_path() : fs::path(dir);
    fs::create_directories(pkgDir);
    for (const auto& [includePath, src] : deps) {
        fs::path targetBase = fs::path(pkgDir) / includePath;
        fs::path targetParent = (includePath.find('/') != std::string::npos) ? targetBase.parent_path() : targetBase;
        std::string moduleName = targetBase.filename().string();
        if (moduleName.size() >= 4 && moduleName.substr(moduleName.size() - 4) == ".nxa") moduleName = moduleName.substr(0, moduleName.size() - 4);
        if (src.substr(0, 5) == "file:") {
            std::string pathStr = src.substr(5);
            fs::path path = fs::path(pathStr);
            if (path.is_relative()) path = (projBase / pathStr).lexically_normal();
            path = fs::absolute(path);
            if (!fs::exists(path)) {
                std::cerr << "[nexapkg] Path not found: " << path.string() << "\n";
                continue;
            }
            fs::create_directories(targetParent);
            if (fs::is_directory(path)) {
                for (const auto& e : fs::directory_iterator(path)) {
                    fs::path dest = targetParent / e.path().filename();
                    if (e.is_directory()) fs::copy(e.path(), dest, fs::copy_options::recursive);
                    else fs::copy_file(e.path(), dest, fs::copy_options::overwrite_existing);
                }
            } else {
                std::string destName = moduleName;
                if (destName.size() < 4 || destName.substr(destName.size() - 4) != ".nxa") destName += ".nxa";
                fs::copy_file(path, targetParent / destName, fs::copy_options::overwrite_existing);
            }
            std::cout << "[nexapkg] Installed " << includePath << " (local)\n";
        } else {
            std::string url = src;
            if (url.find("http") != 0) url = "https://github.com/" + url;
            std::string tmpName = "nexapkg_";
            for (char c : includePath) tmpName += (c == '/') ? '_' : c;
            fs::path tmp = fs::temp_directory_path() / tmpName;
            fs::remove_all(tmp);
            fs::create_directories(tmp);
            std::string cmd = "git clone --depth 1 " + url + " \"" + tmp.string() + "\" 2>/dev/null";
            int ret = std::system(cmd.c_str());
            if (ret != 0) {
                std::cerr << "[nexapkg] Failed to fetch " << includePath << " from " << url << "\n";
                continue;
            }
            fs::create_directories(targetParent);
            for (const auto& e : fs::directory_iterator(tmp)) {
                std::string fn = e.path().filename().string();
                if (fn == ".git") continue;
                fs::path dest = targetParent / fn;
                if (e.is_directory()) fs::copy(e.path(), dest, fs::copy_options::recursive);
                else fs::copy_file(e.path(), dest, fs::copy_options::overwrite_existing);
            }
            fs::remove_all(tmp);
            std::cout << "[nexapkg] Installed " << includePath << "\n";
        }
    }
    return 0;
}

static int cmdList(const std::string& dir) {
    namespace fs = std::filesystem;
    fs::path base = dir.empty() ? fs::current_path() : fs::path(dir);
    fs::path manifest = base / "nexapkg.json";
    if (!fs::exists(manifest)) {
        std::cerr << "[nexapkg] nexapkg.json not found.\n";
        return 1;
    }
    std::map<std::string, std::string> deps;
    parseNexapkgJson(manifest.string(), deps);
    std::cout << "Dependencies:\n";
    for (const auto& [k, v] : deps) std::cout << "  " << k << " <- " << v << "\n";
    return 0;
}

static int run(int argc, char* argv[]) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        std::cout << "nexapkg - Nexa package manager\n";
        std::cout << "Usage: nexapkg init [dir]\n";
        std::cout << "       nexapkg add [as <path>] <pkg> [dir]\n";
        std::cout << "       nexapkg install [--global] [dir]\n";
        std::cout << "       nexapkg list [dir]\n";
        return 0;
    }
    std::string cmd = argv[1];
    std::string arg1 = argc >= 3 ? argv[2] : "";
    std::string arg2 = argc >= 4 ? argv[3] : "";
    if (cmd == "init") return cmdInit(arg1);
    if (cmd == "add") {
        if (arg1.empty()) return 1;
        std::string spec, dir, asPath;
        if (arg1 == "as" && argc >= 5) { asPath = arg2; spec = argv[4]; dir = argc >= 6 ? argv[5] : ""; }
        else if (arg2 == "as" && argc >= 4) { spec = arg1; asPath = argv[3]; dir = argc >= 5 ? argv[4] : ""; }
        else { spec = arg1; dir = arg2; }
        return cmdAdd(spec, dir, asPath);
    }
    if (cmd == "install") {
        bool global = (arg1 == "--global");
        std::string dir, single;
        if (global) {
            if (argc >= 4) { single = argv[3]; dir = ""; }
            else if (argc >= 3) {
                std::string a = argv[2];
                single = (a.find('/') != std::string::npos || (a.size() >= 2 && a.substr(0, 2) == "./")) ? a : "";
                dir = single.empty() ? a : "";
            }
        } else {
            dir = arg1;
            if (argc >= 3 && !arg1.empty() && (arg1.find('/') != std::string::npos || (arg1.size() >= 2 && arg1.substr(0, 2) == "./"))) single = arg1;
        }
        if (!single.empty() && !global && cmdAdd(single, dir, "") != 0) return 1;
        return cmdInstall(dir, global, single);
    }
    if (cmd == "list") return cmdList(arg1);
    std::cerr << "[nexapkg] Unknown command: " << cmd << "\n";
    return 1;
}

}
}
