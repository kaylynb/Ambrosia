#include "compiler/exec_parse.h"
#include "test_paths.h"
#include "compiler/exporter.h"
#include "compiler/package_dump.h"
#include "compiler/script_compiler.h"
#include "core/objects.h"
#include "core/vm.h"
#include "deusex/startup.h"
#include "engine/world.h"
#include "fixtures.h"
#include "ucc_runner.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
    using ucctest::runUcc;

    std::string lowered(std::string text)
    {
        for (char& c : text) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return text;
    }

    std::string normalized(const dx::compiler::DumpedExport& entry)
    {
        static const std::regex dependencyCrc(R"((\{[^}]*\}:[01]):[0-9]+)");
        static const std::regex importTime(R"(InternalTime\[[01]\]:2=[0-9a-f]+ )");
        static const std::regex leadingText(R"(text=\s+)");
        std::string body = std::regex_replace(entry.body, dependencyCrc, "$1");
        body = std::regex_replace(body, importTime, "");
        if (entry.className == "TextBuffer") {
            body = std::regex_replace(body, leadingText, "text=", std::regex_constants::format_first_only);
            while (!body.empty() && (body.back() == '\r' || body.back() == '\n' || body.back() == ' ')) {
                body.pop_back();
            }
        }
        return lowered(body);
    }

    std::map<std::string, dx::compiler::DumpedExport> withCanonicalPalettes(const std::map<std::string, dx::compiler::DumpedExport>& dump)
    {
        static const std::regex paletteRef(R"(Palette\[0\]:5=\{([^}]*)\})");
        std::map<std::string, std::string> names;
        for (const auto& [path, entry] : dump) {
            for (std::sregex_iterator it(entry.body.begin(), entry.body.end(), paletteRef), end; it != end; ++it) {
                names.emplace(lowered((*it)[1].str()), "palette#" + std::to_string(names.size()));
            }
        }
        std::map<std::string, std::string> keys;
        for (const auto& [ref, name] : names) {
            keys[ref.substr(ref.find('.') + 1)] = name;
        }
        std::map<std::string, dx::compiler::DumpedExport> result;
        for (const auto& [path, entry] : dump) {
            dx::compiler::DumpedExport copy = entry;
            copy.body.clear();
            auto last = entry.body.cbegin();
            for (std::sregex_iterator it(entry.body.begin(), entry.body.end(), paletteRef), end; it != end; ++it) {
                copy.body.append(last, (*it)[1].first);
                copy.body += names[lowered((*it)[1].str())];
                last = (*it)[1].second;
            }
            copy.body.append(last, entry.body.cend());
            auto key = keys.find(path);
            result[key != keys.end() ? key->second : path] = copy;
        }
        return result;
    }

    std::vector<std::string> editPackages()
    {
        std::ifstream in(std::filesystem::path(dx::test::gameData()) / "System" / "DeusEx.ini");
        std::vector<std::string> packages;
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("EditPackages=", 0) != 0) {
                continue;
            }
            std::string name = line.substr(13);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) {
                name.pop_back();
            }
            packages.push_back(name);
        }
        return packages;
    }

    std::map<std::string, dx::compiler::DumpedExport> dumpByPath(const std::filesystem::path& file)
    {
        std::map<std::string, dx::compiler::DumpedExport> result;
        for (auto& [path, entry] : dx::compiler::dumpPackage(file, std::filesystem::path(dx::test::gameData()) / "System")) {
            result[lowered(path)] = entry;
        }
        return result;
    }
}

TEST_CASE("exported default properties use the UnrealEd format", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UClass* pawn = dx::objects::findClass("DeusEx.ScriptedPawn");
    REQUIRE(pawn != nullptr);
    dx::UClass* parent = pawn->superClass();
    std::string text = dx::compiler::exportProperties(pawn, pawn->defaultObject->data, parent, parent->defaultObject->data, 4);
    CHECK(text.find("     Restlessness=0.500000\r\n") != std::string::npos);
    CHECK(text.find("     Orders=Wandering\r\n") != std::string::npos);
    CHECK(text.find("     RaiseAlarm=RAISEALARM_BeforeFleeing\r\n") != std::string::npos);
}

TEST_CASE("exported floats that six decimals cannot represent keep full precision", "[data]")
{
    dx::engine::initialize(dx::test::gameData());
    dx::UClass* cat = dx::objects::findClass("DeusEx.Cat");
    REQUIRE(cat != nullptr);
    dx::UClass* parent = cat->superClass();
    std::string text = dx::compiler::exportProperties(cat, cat->defaultObject->data, parent, parent->defaultObject->data, 4);
    std::string key = "     WalkingSpeed=";
    size_t at = text.find(key);
    REQUIRE(at != std::string::npos);
    std::string value = text.substr(at + key.size(), text.find("\r\n", at) - at - key.size());
    CHECK(value == "0.111111112");
    float parsed = static_cast<float>(std::atof(value.c_str()));
    uint32_t bits = 0;
    std::memcpy(&bits, &parsed, sizeof(bits));
    CHECK(bits == 0x3de38e39u);
}

TEST_CASE("ucc make rebuilds every EditPackage from exported sources", "[data]")
{
    const char* xdg = std::getenv("XDG_DATA_HOME");
    REQUIRE(xdg != nullptr);
    std::filesystem::path home = std::filesystem::path(xdg) / "ucc-make";
    std::filesystem::remove_all(home);
    std::filesystem::path user = home / "ambrosia";
    std::vector<std::string> packages = editPackages();
    REQUIRE(packages.size() == 19);
    CHECK(packages.front() == "Core");
    for (const std::string& package : packages) {
        INFO(package);
        REQUIRE(runUcc(home, "exportsources " + package + ".u \"" + user.string() + "\"") == 0);
    }
    CHECK(std::filesystem::exists(user / "Engine" / "Fonts" / "LargeFont.txt"));
    REQUIRE(runUcc(home, "make") == 0);
    const std::set<std::string> lossyMeshes = { "DeusExCharacters", "MPCharacters", "DeusExDeco" };
    for (const std::string& package : packages) {
        INFO(package);
        std::filesystem::path shipped = std::filesystem::path(dx::test::gameData()) / "System" / (package + ".u");
        std::filesystem::path built = user / "System" / (package + ".u");
        REQUIRE(std::filesystem::exists(built));
        auto expected = withCanonicalPalettes(dumpByPath(shipped));
        auto actual = withCanonicalPalettes(dumpByPath(built));
        CHECK(actual.size() == expected.size());
        int mismatches = 0;
        std::string first;
        for (auto& [path, entry] : expected) {
            auto it = actual.find(path);
            bool same = it != actual.end() && it->second.className == entry.className && lowered(it->second.super) == lowered(entry.super) && it->second.flags == entry.flags;
            if (same && lossyMeshes.count(package) && entry.className == "LodMesh") {
                std::string left = normalized(entry);
                std::string right = normalized(it->second);
                same = left.substr(0, left.find("\nrest=")) == right.substr(0, right.find("\nrest="));
            } else if (same) {
                same = normalized(it->second) == normalized(entry);
            }
            if (!same) {
                mismatches++;
                if (first.empty()) {
                    first = path;
                }
            }
        }
        INFO("first mismatch " << first);
        CHECK(mismatches == 0);
    }
    auto ipServer = dumpByPath(user / "System" / "IpServer.u");
    CHECK(ipServer["udpserverquery"].body.find("{IpServer.UdpServerQuery}:1:1727771771,{IpDrv.UdpLink}:1:2233407030,") != std::string::npos);
    auto engine = dumpByPath(user / "System" / "Engine.u");
    CHECK(engine["largefont"].className == "Font");
    CHECK(engine["actor.ambientsound"].body.find("type={Engine.Sound}") != std::string::npos);
    std::filesystem::remove_all(home);
}

TEST_CASE("ucc make rebuilds Core on its own", "[data]")
{
    const char* xdg = std::getenv("XDG_DATA_HOME");
    REQUIRE(xdg != nullptr);
    std::filesystem::path home = std::filesystem::path(xdg) / "ucc-core";
    std::filesystem::remove_all(home);
    std::filesystem::path user = home / "ambrosia";
    std::filesystem::create_directories(user);
    {
        std::ifstream in(std::filesystem::path(dx::test::gameData()) / "System" / "DeusEx.ini", std::ios::binary);
        std::ofstream out(user / "DeusEx.ini", std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("EditPackages=", 0) != 0 || line.rfind("EditPackages=Core", 0) == 0) {
                out << line << "\n";
            }
        }
    }
    REQUIRE(runUcc(home, "exportsources Core.u \"" + user.string() + "\"") == 0);
    std::filesystem::path log = home / "make.log";
    REQUIRE(runUcc(home, "make", log) == 0);
    std::stringstream output;
    {
        // Closed again before the end, as Windows can't remove a directory holding an open file.
        std::ifstream in(log);
        output << in.rdbuf();
    }
    CHECK(output.str().find("Building Core from ") != std::string::npos);
    CHECK(output.str().find("--------------------Engine") == std::string::npos);
    auto expected = dumpByPath(std::filesystem::path(dx::test::gameData()) / "System" / "Core.u");
    auto actual = dumpByPath(user / "System" / "Core.u");
    REQUIRE(actual.size() == expected.size());
    int mismatches = 0;
    for (auto& [path, entry] : expected) {
        auto it = actual.find(path);
        if (it == actual.end() || it->second.className != entry.className || it->second.flags != entry.flags || normalized(it->second) != normalized(entry)) {
            mismatches++;
        }
    }
    CHECK(mismatches == 0);
    CHECK(actual["object"].className == "Class");
    std::filesystem::remove_all(home);
}

TEST_CASE("ucc make reports script errors with file and line and writes no package", "[data]")
{
    struct BrokenScript {
        const char* source;
        const char* error;
    };

    const BrokenScript cases[] = {
        { "class Broken extends Actor;\r\n\r\nvar int Count\r\nvar int Other;\r\n", "Broken.uc(4) : Error, Missing ';' before 'var'" },
        { "class Broken extends Actor;\r\n\r\nfunction Touch(Actor Other)\r\n{\r\n\tUndefinedThing = 1;\r\n}\r\n", "Broken.uc(5) : Error, 'UndefinedThing': Bad command or expression" },
        { "class Broken extends Actor;\r\n\r\nfunction PostBeginPlay()\r\n{\r\n\tlocal int Value;\r\n\r\n\tValue = Self;\r\n}\r\n", "Broken.uc(7) : Error, Type mismatch in '='" },
        { "class Broken extends Actor;\r\n\r\nvar int Counts[0];\r\n", "Broken.uc(3) : Error, Variable declaration Counts: Illegal array size 0" },
        { "class Broken extends Actor;\r\n\r\nvar int Counts[-1];\r\n", "Broken.uc(3) : Error, Variable declaration Counts: Illegal array size -1" },
        { "class Broken extends Actor;\r\n\r\nvar int Counts[2049];\r\n", "Broken.uc(3) : Error, Variable declaration Counts: Illegal array size 2049" },
        { "class Broken expands Baar;\r\n", "Superclass Baar of class Broken not found" },
        { "class Broken extends Actor;\r\n\r\nvar", "Broken.uc(4) : Error, Variable declaration: Missing variable type" },
        { "class Broken extends Actor", "Broken.uc(2) : Error, Missing ';' in 'Class'" },
    };
    const char* xdg = std::getenv("XDG_DATA_HOME");
    REQUIRE(xdg != nullptr);
    std::filesystem::path home = std::filesystem::path(xdg) / "ucc-errors";
    std::filesystem::path user = home / "ambrosia";
    for (const BrokenScript& broken : cases) {
        INFO(broken.error);
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(user / "BrokenMod" / "Classes");
        {
            std::ifstream in(std::filesystem::path(dx::test::gameData()) / "System" / "DeusEx.ini", std::ios::binary);
            std::stringstream ini;
            ini << in.rdbuf();
            std::string text = ini.str();
            size_t last = text.rfind("EditPackages=");
            REQUIRE(last != std::string::npos);
            text.insert(text.find('\n', last) + 1, "EditPackages=BrokenMod\r\n");
            std::ofstream out(user / "DeusEx.ini", std::ios::binary);
            out << text;
        }
        {
            std::ofstream out(user / "BrokenMod" / "Classes" / "Broken.uc", std::ios::binary);
            out << broken.source;
        }
        std::filesystem::path log = home / "make.log";
        CHECK(runUcc(home, "make", log) != 0);
        std::ifstream in(log);
        std::stringstream output;
        output << in.rdbuf();
        CHECK(output.str().find(broken.error) != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(user / "System" / "BrokenMod.u"));
    }
    std::filesystem::remove_all(home);
}

namespace {
    dx::UClass* compileInProcess(const char* package, const char* className, const std::string& source, std::string& log)
    {
        dx::compiler::PackageCompiler compiler(dx::objects::packageObject(dx::Name(package)), [&log](const std::string& line) {
            log += line + "\n";
        });
        dx::UClass* cls = compiler.importClass(std::filesystem::path(package) / "Classes" / (std::string(className) + ".uc"), source);
        if (!cls || !compiler.compile()) {
            return nullptr;
        }
        return cls;
    }
}

TEST_CASE("hex literals are 32-bit and saturate above that like the original", "[.][editor-scripts][data]")
{
    dx::test::ScriptEditing editing;
    std::string log;
    dx::UClass* cls = compileInProcess("HexLiteralTest", "HexLiterals",
        "class HexLiterals extends Object;\r\n\r\nfunction int AllBits()\r\n{\r\n\treturn 0xFFFFFFFF;\r\n}\r\n\r\nfunction int TopBit()\r\n{\r\n\treturn 0x80000000;\r\n}\r\n\r\n"
        "function int Wide()\r\n{\r\n\treturn 0x100000000;\r\n}\r\n\r\nfunction int Huge()\r\n{\r\n\treturn 0x123456789ABCDEF01;\r\n}\r\n",
        log);
    INFO(log);
    REQUIRE(cls != nullptr);
    dx::UObject* object = dx::objects::newObject(cls, nullptr, dx::Name(), 0);
    CHECK(dx::vm::Call(object, dx::Name("AllBits")).result<int32_t>() == -1);
    CHECK(dx::vm::Call(object, dx::Name("TopBit")).result<int32_t>() == INT32_MIN);
    CHECK(dx::vm::Call(object, dx::Name("Wide")).result<int32_t>() == -1);
    CHECK(dx::vm::Call(object, dx::Name("Huge")).result<int32_t>() == -1);
}

TEST_CASE("an array size of one compiles like the original", "[.][editor-scripts][data]")
{
    dx::test::ScriptEditing editing;
    std::string log;
    dx::UClass* cls = compileInProcess("ArrayOneTest", "ArrayOne", "class ArrayOne extends Object;\r\n\r\nvar int Single[1];\r\nvar int After;\r\n", log);
    INFO(log);
    REQUIRE(cls != nullptr);
    dx::UProperty* single = cls->findProperty(dx::Name("Single"));
    REQUIRE(single != nullptr);
    CHECK(single->arrayDim == 1);
}

TEST_CASE("a negative default properties subscript is rejected like an out of range one", "[.][editor-scripts][data]")
{
    dx::test::ScriptEditing editing;
    std::string log;
    dx::UClass* cls = compileInProcess("NegativeDefaultTest", "NegativeDefault",
        "class NegativeDefault extends Object;\r\n\r\nvar int Before;\r\nvar int Counts[2];\r\n\r\ndefaultproperties\r\n{\r\n     Counts(-1)=5\r\n     Counts(1)=7\r\n}\r\n", log);
    INFO(log);
    REQUIRE(cls != nullptr);
    CHECK(log.find("Out of bound array default property") != std::string::npos);
    dx::UObject* defaults = cls->defaultObject;
    REQUIRE(defaults != nullptr);
    CHECK(*reinterpret_cast<int32_t*>(defaults->propertyAddress(dx::Name("Before"))) == 0);
    CHECK(reinterpret_cast<int32_t*>(defaults->propertyAddress(dx::Name("Counts")))[1] == 7);
}

namespace {
    // Copies the file, or every file of the directory, at a relative path under from to the same path under to.
    // Paths compare without case, as exported sources mix "Textures" and "TEXTURES".
    size_t copySources(const std::filesystem::path& from, const std::filesystem::path& to, const std::string& relative)
    {
        std::string prefix = relative;
        for (char& c : prefix) {
            if (c == '\\') {
                c = '/';
            }
        }
        std::string wanted = lowered(prefix);
        size_t copied = 0;
        for (const auto& item : std::filesystem::recursive_directory_iterator(from)) {
            std::string path = item.path().lexically_relative(from).generic_string();
            std::string key = lowered(path);
            if (item.is_regular_file() && (key == wanted || key.rfind(wanted + "/", 0) == 0)) {
                std::filesystem::path target = to / (prefix + path.substr(prefix.size()));
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::copy_file(item.path(), target, std::filesystem::copy_options::overwrite_existing);
                copied++;
            }
        }
        return copied;
    }

    // Exports the sources of a shipped package, builds a one-class package from some of its #exec lines and the
    // files they read with ucc make, and returns the dump of the built package.
    std::map<std::string, dx::compiler::DumpedExport> makeFromExecs(const std::string& testName, const std::string& shipped, const std::string& mod, const std::string& execs, const std::vector<std::string>& files, const std::string& builtPackage = {})
    {
        // ucc (or the child run) needs the data too; this fails saying it is missing, not with an exit code.
        dx::test::gameData();
        const char* xdg = std::getenv("XDG_DATA_HOME");
        REQUIRE(xdg != nullptr);
        std::filesystem::path home = std::filesystem::path(xdg) / testName;
        std::filesystem::remove_all(home);
        std::filesystem::path sources = home / "sources";
        REQUIRE(runUcc(home, "exportsources " + shipped + ".u \"" + sources.string() + "\"") == 0);
        std::filesystem::path user = home / "ambrosia";
        std::filesystem::create_directories(user / mod / "Classes");
        {
            std::ifstream in(std::filesystem::path(dx::test::gameData()) / "System" / "DeusEx.ini", std::ios::binary);
            std::stringstream ini;
            ini << in.rdbuf();
            std::string text = ini.str();
            size_t last = text.rfind("EditPackages=");
            REQUIRE(last != std::string::npos);
            text.insert(text.find('\n', last) + 1, "EditPackages=" + mod + "\r\n");
            std::ofstream out(user / "DeusEx.ini", std::ios::binary);
            out << text;
        }
        {
            std::ofstream out(user / mod / "Classes" / (mod + ".uc"), std::ios::binary);
            out << "class " << mod << " expands Object\r\n\tabstract;\r\n\r\n"
                << execs << "\r\ndefaultproperties\r\n{\r\n}\r\n";
        }
        for (const std::string& file : files) {
            INFO(file);
            REQUIRE(copySources(sources / shipped, user / mod, file) > 0);
        }
        std::filesystem::path log = home / "make.log";
        int result = runUcc(home, "make", log);
        std::stringstream output;
        {
            std::ifstream in(log);
            output << in.rdbuf();
        }
        INFO(output.str());
        REQUIRE(result == 0);
        auto dump = dumpByPath(user / "System" / ((builtPackage.empty() ? mod : builtPackage) + ".u"));
        std::filesystem::remove_all(home);
        return dump;
    }

    // An export's dump with references into either package written the same way and palettes left out,
    // as the importer names its palettes by creation order.
    std::string comparable(const dx::compiler::DumpedExport& entry, const std::string& shipped, const std::string& mod)
    {
        static const std::regex paletteRef(R"(palette\[0\]:5=\{[^}]*\})");
        std::string body = normalized(entry);
        body = std::regex_replace(body, std::regex("\\{(" + lowered(shipped) + "|" + lowered(mod) + ")\\."), "{package.");
        return std::regex_replace(body, paletteRef, "palette[0]:5={palette}");
    }

    std::string paletteOf(const dx::compiler::DumpedExport& entry)
    {
        static const std::regex paletteRef(R"(Palette\[0\]:5=\{[^.}]*\.([^}]*)\})");
        std::smatch match;
        return std::regex_search(entry.body, match, paletteRef) ? lowered(match[1].str()) : std::string();
    }

    void checkSameExports(const std::map<std::string, dx::compiler::DumpedExport>& built, const std::string& shipped, const std::string& mod, const std::vector<std::string>& paths)
    {
        auto expected = dumpByPath(std::filesystem::path(dx::test::gameData()) / "System" / (shipped + ".u"));
        for (const std::string& path : paths) {
            INFO(path);
            auto want = expected.find(path);
            auto got = built.find(path);
            REQUIRE(want != expected.end());
            REQUIRE(got != built.end());
            CHECK(got->second.className == want->second.className);
            CHECK(got->second.flags == want->second.flags);
            CHECK(comparable(got->second, shipped, mod) == comparable(want->second, shipped, mod));
            std::string wantPalette = paletteOf(want->second);
            std::string gotPalette = paletteOf(got->second);
            CHECK(wantPalette.empty() == gotPalette.empty());
            if (!wantPalette.empty() && !gotPalette.empty()) {
                REQUIRE(expected.count(wantPalette) == 1);
                REQUIRE(built.count(gotPalette) == 1);
                CHECK(normalized(built.at(gotPalette)) == normalized(expected.at(wantPalette)));
            }
        }
    }
}

TEST_CASE("a texture imported by #exec matches the shipped one", "[data]")
{
    auto built = makeFromExecs("ucc-texture-import", "UWindow", "TexImportTest",
        "#exec TEXTURE IMPORT NAME=MenuTick FILE=Textures\\MenuTick.bmp GROUP=\"Icons\" FLAGS=2 MIPS=OFF\r\n",
        { "Textures\\MenuTick.bmp" });
    checkSameExports(built, "UWindow", "TexImportTest", { "icons.menutick" });
}

TEST_CASE("a mesh imported by #exec matches the shipped one", "[data]")
{
    auto built = makeFromExecs("ucc-mesh-import", "DeusExItems", "MeshImportTest",
        "#exec MESH IMPORT MESH=Binoculars ANIVFILE=Models\\Binoculars_a.3d DATAFILE=Models\\Binoculars_d.3d ZEROTEX=1\r\n"
        "#exec MESHMAP SCALE MESHMAP=Binoculars X=0.00390625 Y=0.00390625 Z=0.00390625\r\n"
        "#exec MESH SEQUENCE MESH=Binoculars SEQ=All\t\tSTARTFRAME=0\tNUMFRAMES=1\r\n"
        "#exec MESH SEQUENCE MESH=Binoculars SEQ=Still\tSTARTFRAME=0\tNUMFRAMES=1\r\n"
        "#exec TEXTURE IMPORT NAME=BinocularsTex1 FILE=Models\\Binoculars.pcx GROUP=\"Skins\"\r\n"
        "#exec MESHMAP SETTEXTURE MESHMAP=Binoculars NUM=0 TEXTURE=BinocularsTex1\r\n",
        { "Models\\Binoculars_a.3d", "Models\\Binoculars_d.3d", "Models\\Binoculars.pcx" });
    checkSameExports(built, "DeusExItems", "MeshImportTest", { "binoculars", "skins.binocularstex1" });
}

namespace {
    std::string conName(const dx::compiler::DumpedExport& entry)
    {
        static const std::regex name(R"(conName\[0\]:6='([^']*)')");
        std::smatch match;
        return std::regex_search(entry.body, match, name) ? match[1].str() : std::string();
    }

    // A conversation object's dump with each reference into its own package replaced by the referenced object's
    // dump, so two packages compare by content whatever the importer named the objects. References to another
    // conversation are written as its conName; references outside the package keep their name only.
    std::string structural(const std::map<std::string, dx::compiler::DumpedExport>& dump, const std::string& path, std::set<std::string>& open)
    {
        static const std::regex ref(R"(\{([^}.]*)\.([^}]*)\})");
        const dx::compiler::DumpedExport& entry = dump.at(path);
        std::string result = entry.className + ":";
        auto last = entry.body.cbegin();
        for (std::sregex_iterator it(entry.body.begin(), entry.body.end(), ref), end; it != end; ++it) {
            result.append(last, (*it)[0].first);
            last = (*it)[0].second;
            std::string target = lowered((*it)[2].str());
            auto found = dump.find(target);
            if (found == dump.end()) {
                result += "{outside." + target + "}";
            } else if (found->second.className == "Conversation" && !open.empty()) {
                result += "{conversation " + conName(found->second) + "}";
            } else if (open.count(target)) {
                result += "{cycle}";
            } else {
                open.insert(target);
                result += "{" + structural(dump, target, open) + "}";
                open.erase(target);
            }
        }
        result.append(last, entry.body.cend());
        return result;
    }

    // Conversation names repeat across missions (DL_Entry), so each name keeps every conversation that has it.
    std::map<std::string, std::vector<std::string>> conversationsByName(const std::map<std::string, dx::compiler::DumpedExport>& dump)
    {
        static const std::regex soundId(R"(soundID\[0\]:2=[0-9a-f]+)");
        std::map<std::string, std::vector<std::string>> result;
        for (const auto& [path, entry] : dump) {
            if (entry.className == "Conversation") {
                std::set<std::string> open;
                // Sound IDs number the sounds of the whole audio package, which in the shipped build holds every
                // conversation file of the mission.
                result[conName(entry)].push_back(std::regex_replace(structural(dump, path, open), soundId, "soundID"));
            }
        }
        return result;
    }
}

TEST_CASE("conversations imported by #exec match the shipped ones", "[data]")
{
    auto built = makeFromExecs("ucc-conversation-import", "DeusExConversations", "ConImportTest",
        "#exec CONVERSATION IMPORT FILE=\"Mission8_Infolink.Con\"\r\n",
        { "Mission8_Infolink.Con", "Audio\\Mission08" }, "ConImportTestText");
    auto actual = conversationsByName(built);
    auto expected = conversationsByName(dumpByPath(std::filesystem::path(dx::test::gameData()) / "System" / "DeusExConText.u"));
    size_t count = 0;
    for (const auto& [name, contents] : actual) {
        INFO(name);
        auto shipped = expected.find(name);
        REQUIRE(shipped != expected.end());
        for (const std::string& content : contents) {
            CHECK(std::find(shipped->second.begin(), shipped->second.end(), content) != shipped->second.end());
            count++;
        }
    }
    CHECK(count == 9);
}

TEST_CASE("exec keys match only at a word start like appStrfind")
{
    std::string value;
    REQUIRE(dx::compiler::parseString("FONT IMPORT FontName=Arial Name=Big", "Name=", value));
    CHECK(value == "Big");
    CHECK(dx::compiler::parseString("NAME=Tex FILE=a.pcx", "name=", value));
    CHECK(value == "Tex");
    CHECK_FALSE(dx::compiler::parseString("MESH IMPORT ANIVFILE=a.3d DATAFILE=d.3d", "FILE=", value));
    CHECK(dx::compiler::parseString("X=1,FILE=b.pcx", "FILE=", value));
    CHECK(value == "b.pcx");
    CHECK(dx::compiler::parseString("_FILE=c.pcx", "FILE=", value));
    CHECK(value == "c.pcx");
    CHECK(dx::compiler::findKey("subclass Foo", "class") == std::string_view::npos);
    CHECK(dx::compiler::findKey("class Foo extends Bar;", "extends") == 10);
}

TEST_CASE("deeply nested expressions fail with a compile error instead of overflowing the stack", "[.][editor-scripts][data]")
{
    std::string source = "class DeepNesting extends Object;\r\n\r\nfunction int Deep()\r\n{\r\n\treturn " + std::string(300, '(') + "1" + std::string(300, ')') + ";\r\n}\r\n";
    std::string log;
    dx::test::ScriptEditing editing;
    {
        dx::compiler::PackageCompiler compiler(dx::objects::packageObject(dx::Name("DeepNestingTest")), [&log](const std::string& line) {
            log += line + "\n";
        });
        dx::UClass* deep = compiler.importClass(std::filesystem::path("DeepNestingTest") / "Classes" / "DeepNesting.uc", source);
        REQUIRE(deep != nullptr);
        CHECK_FALSE(compiler.compile());
        // Retired so later compiles in this process do not parse it again.
        deep->destroyed = true;
    }
    CHECK(log.find("Expression nested too deeply (more than 256 levels)") != std::string::npos);
    std::string shallow;
    dx::UClass* cls = compileInProcess("ShallowNestingTest", "ShallowNesting", "class ShallowNesting extends Object;\r\n\r\nfunction int Shallow()\r\n{\r\n\treturn " + std::string(100, '(') + "1" + std::string(100, ')') + ";\r\n}\r\n", shallow);
    INFO(shallow);
    CHECK(cls != nullptr);
}

TEST_CASE("a defaults line ending in a backslash escape or an object name stops at the end of the line", "[.][editor-scripts][data]")
{
    dx::test::ScriptEditing editing;
    std::string log;
    dx::UClass* cls = compileInProcess("DefaultsEndTest", "DefaultsEnd",
        "class DefaultsEnd extends Object;\r\n\r\nstruct Holder\r\n{\r\n\tvar Class<Object> Kind;\r\n\tvar int After;\r\n};\r\n\r\nvar string Text;\r\nvar Holder Held;\r\nvar int Count;\r\n\r\n"
        "defaultproperties\r\n{\r\n     Text=\"ab\\\r\n     Held=(Kind=Class'Core.Object'\r\n     Count=3\r\n}\r\n",
        log);
    INFO(log);
    REQUIRE(cls != nullptr);
    CHECK(*reinterpret_cast<int32_t*>(cls->defaultObject->propertyAddress(dx::Name("Count"))) == 3);
}

TEST_CASE("ucc exportsources fails when it cannot create the output directory", "[data]")
{
    const char* xdg = std::getenv("XDG_DATA_HOME");
    REQUIRE(xdg != nullptr);
    std::filesystem::path home = std::filesystem::path(xdg) / "ucc-unwritable";
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    {
        std::ofstream blocker(home / "blocker");
        blocker << "not a directory";
    }
    CHECK(runUcc(home, "exportsources Core.u \"" + (home / "blocker" / "out").string() + "\"") != 0);
    std::filesystem::remove_all(home);
}