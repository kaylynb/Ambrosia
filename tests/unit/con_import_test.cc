#include "compiler/asset_package.h"
#include "test_paths.h"
#include "compiler/con_file.h"
#include "compiler/script_package.h"
#include "core/config.h"
#include "core/objects.h"
#include "deusex/startup.h"
#include "engine/resources.h"
#include "fixtures.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// How the two readers of .con files treat files the conversation editor would not write: the
// package importer behind #exec CONVERSATION IMPORT, and readConFile behind the editor. The
// values are what the code gives today.

namespace {
    class ConBytes {
    public:
        ConBytes& integer(int32_t value)
        {
            for (int i = 0; i < 4; i++) {
                data.push_back(static_cast<uint8_t>(static_cast<uint32_t>(value) >> (8 * i)));
            }
            return *this;
        }

        ConBytes& raw(const std::string& value)
        {
            data.insert(data.end(), value.begin(), value.end());
            return *this;
        }

        ConBytes& string(const std::string& value)
        {
            integer(static_cast<int32_t>(value.size()));
            return raw(value);
        }

        ConBytes& actor(int32_t id, const std::string& name)
        {
            integer(id);
            return id > -1 ? string(name) : *this;
        }

        ConBytes& date()
        {
            return raw(std::string(8, '\0'));
        }

        std::vector<uint8_t> data;
    };

    ConBytes header(int32_t version, const std::string& signature = "Deus Ex Conversation File")
    {
        ConBytes bytes;
        bytes.raw(signature).raw(std::string(1, '\0')).integer(version);
        bytes.date().string("Author").date().string("Author");
        bytes.string("Q").string("File notes");
        bytes.integer(1).integer(90);
        bytes.integer(0);
        bytes.integer(0);
        bytes.integer(1);
        return bytes;
    }

    void conversation(ConBytes& bytes, int32_t eventCount)
    {
        bytes.integer(0).integer(7).string("Talk").string("Characterised");
        bytes.date().string("Author").date().string("Author");
        bytes.actor(1, "Paul");
        bytes.integer(0).string("Notes");
        for (int i = 0; i < 6; i++) {
            bytes.integer(0);
        }
        bytes.integer(0).integer(1).integer(0).integer(0).integer(0);
        bytes.integer(0);
        bytes.integer(eventCount);
    }

    void event(ConBytes& bytes, int32_t type, const std::string& label)
    {
        bytes.integer(0).integer(0).integer(type).string(label);
    }

    void speech(ConBytes& bytes, int32_t speakerId, const std::string& speaker, int32_t toId, const std::string& to, const std::string& line)
    {
        bytes.actor(speakerId, speaker).actor(toId, to).string(line).string("Audio");
        bytes.integer(0).integer(0).integer(0);
    }

    template <typename T>
    T& field(dx::UObject* object, const char* name)
    {
        uint8_t* address = object->propertyAddress(dx::Name(name));
        REQUIRE(address != nullptr);
        return *reinterpret_cast<T*>(address);
    }

    std::string text(dx::UObject* object, const char* name)
    {
        return field<std::string>(object, name);
    }

    dx::UObject* reference(dx::UObject* object, const char* name)
    {
        return field<dx::UObject*>(object, name);
    }

    std::vector<dx::UObject*> importedEvents(const std::string& prefix)
    {
        dx::UObject* package = dx::objects::packageObject(dx::Name(prefix + "Text"));
        dx::UClass* cls = dx::objects::findClass("ConSys.Conversation");
        std::vector<dx::UObject*> result;
        for (dx::UObject* object : dx::compiler::packageContents(package)) {
            if (object->cls == cls) {
                for (dx::UObject* item = reference(object, "eventList"); item; item = reference(item, "nextEvent")) {
                    result.push_back(item);
                }
            }
        }
        return result;
    }

    struct Import {
        explicit Import(const std::string& name)
            : prefix(name)
            , owner(name)
            , text(name + "Text")
            , audio(name + "AudioQ")
        {
            dx::engine::initialize(dx::test::gameData());
            root = dx::config::userDirectory() / "con-import-test" / prefix;
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root / prefix);
        }

        std::vector<std::string> run(const std::vector<uint8_t>& bytes)
        {
            {
                std::ofstream out(root / prefix / "Q.con", std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            std::vector<std::string> log;
            dx::compiler::runAssetCommand(dx::objects::packageObject(dx::Name(prefix)), root / prefix, "CONVERSATION IMPORT FILE=\"Q.con\"", log);
            for (std::string& line : log) {
                std::replace(line.begin(), line.end(), '\\', '/');
            }
            return log;
        }

        std::string prefix;
        std::filesystem::path root;
        dx::test::PackageGuard owner;
        dx::test::PackageGuard text;
        dx::test::PackageGuard audio;
    };

    bool endsWith(const std::string& value, const std::string& end)
    {
        return value.size() >= end.size() && value.compare(value.size() - end.size(), end.size(), end) == 0;
    }
}

TEST_CASE("the package importer takes a .con without its signature, which the editor reader refuses", "[data]")
{
    ConBytes bytes = header(0x1c, "Not a Deus Ex convo file!");
    conversation(bytes, 1);
    event(bytes, 18, "Only");

    dx::compiler::ConFile file;
    std::string error;
    CHECK_FALSE(dx::compiler::readConFile(bytes.data, file, &error));
    CHECK(error == "not a Deus Ex conversation file");

    Import import("ConQuirkSignature");
    std::vector<std::string> log = import.run(bytes.data);
    CHECK(log.empty());
    std::vector<dx::UObject*> list = importedEvents(import.prefix);
    REQUIRE(list.size() == 1);
    CHECK(list[0]->cls->name.str() == "ConEventEnd");
    CHECK(text(list[0], "Label") == "Only");
    CHECK(std::filesystem::exists(import.root / "System" / "ConQuirkSignatureText.u"));
}

TEST_CASE("both .con readers refuse a version older than 0x1a", "[data]")
{
    ConBytes bytes = header(0x19);
    conversation(bytes, 0);

    dx::compiler::ConFile file;
    std::string error;
    CHECK_FALSE(dx::compiler::readConFile(bytes.data, file, &error));
    CHECK(error == "obsolete conversation file version 25");

    Import import("ConQuirkObsolete");
    std::vector<std::string> log = import.run(bytes.data);
    REQUIRE(log.size() == 1);
    CHECK(log[0].starts_with("CONIMPORT WARNING: Attempt to import obsolete conversation file "));
    CHECK(endsWith(log[0], "/Q.con"));
    CHECK_FALSE(std::filesystem::exists(import.root / "System" / "ConQuirkObsoleteText.u"));
}

TEST_CASE("the package importer skips an unknown event type without its payload and keeps only the low byte of the type", "[data]")
{
    ConBytes bytes = header(0x1c);
    conversation(bytes, 3);
    event(bytes, 40, "Skipped");
    event(bytes, 256, "Wide");
    speech(bytes, 3, "Paul", 2, "JCDenton", "Spoken by a type of 256.");
    event(bytes, 18, "Last");

    dx::compiler::ConFile file;
    std::string error;
    CHECK_FALSE(dx::compiler::readConFile(bytes.data, file, &error));
    CHECK(error == "unknown event type 40 in Talk");
    REQUIRE(file.conversations.size() == 1);
    CHECK(file.conversations[0].events.empty());

    Import import("ConQuirkTypes");
    std::vector<std::string> log = import.run(bytes.data);
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "CONIMPORT WARNING: unknown event type 40");
    CHECK(log[1].starts_with("ImportSpeechAudio Failed: "));
    CHECK(endsWith(log[1], "ConQuirkTypes/Audio/Q/Paul/Talk/Paul01.mp3"));
    std::vector<dx::UObject*> list = importedEvents(import.prefix);
    REQUIRE(list.size() == 2);
    CHECK(list[0]->cls->name.str() == "ConEventSpeech");
    CHECK(field<uint8_t>(list[0], "eventType") == 0);
    CHECK(text(list[0], "Label") == "Wide");
    CHECK(text(reference(list[0], "ConSpeech"), "speech") == "Spoken by a type of 256.");
    CHECK(list[1]->cls->name.str() == "ConEventEnd");
    CHECK(text(list[1], "Label") == "Last");
}

TEST_CASE("the package importer leaves a speaker of -1 empty and counts audio lines by actor id", "[data]")
{
    ConBytes bytes = header(0x1c);
    conversation(bytes, 4);
    event(bytes, 0, "");
    speech(bytes, 3, "Paul", -1, "", "First.");
    event(bytes, 0, "");
    speech(bytes, -1, "", 2, "JCDenton", "Second.");
    event(bytes, 0, "");
    speech(bytes, 3, "Paul", 2, "JCDenton", "Third.");
    event(bytes, 0, "");
    speech(bytes, 4, "Paul", 2, "JCDenton", "Fourth.");

    dx::compiler::ConFile file;
    REQUIRE(dx::compiler::readConFile(bytes.data, file, nullptr));
    REQUIRE(file.conversations[0].events.size() == 4);
    CHECK(file.conversations[0].events[0].speakingTo.empty());
    CHECK(file.conversations[0].events[1].speaker.empty());

    Import import("ConQuirkActors");
    std::vector<std::string> log = import.run(bytes.data);
    REQUIRE(log.size() == 4);
    CHECK(endsWith(log[0], "/Paul/Talk/Paul01.mp3"));
    CHECK(endsWith(log[1], "/Paul/Talk/01.mp3"));
    CHECK(endsWith(log[2], "/Paul/Talk/Paul02.mp3"));
    CHECK(endsWith(log[3], "/Paul/Talk/Paul01.mp3"));
    std::vector<dx::UObject*> list = importedEvents(import.prefix);
    REQUIRE(list.size() == 4);
    CHECK(text(list[0], "speakerName") == "Paul");
    CHECK(text(list[0], "speakingToName").empty());
    CHECK(text(list[1], "speakerName").empty());
    CHECK(text(list[1], "speakingToName") == "JCDenton");

    // An actor of -1 leaves the field at the class default, which is empty for every actor field.
    const std::pair<const char*, const char*> actorFields[] = {
        { "Conversation", "conOwnerName" },
        { "ConEventSpeech", "speakerName" },
        { "ConEventSpeech", "speakingToName" },
        { "ConEventTransferObject", "fromName" },
        { "ConEventTransferObject", "toName" },
        { "ConEventAnimation", "eventOwnerName" },
        { "ConEventTrade", "eventOwnerName" },
    };
    for (const auto& [className, fieldName] : actorFields) {
        INFO(className << "." << fieldName);
        dx::UClass* cls = dx::objects::findClass(std::string("ConSys.") + className);
        REQUIRE(cls != nullptr);
        REQUIRE(cls->defaultObject != nullptr);
        CHECK(text(cls->defaultObject, fieldName).empty());
    }
}

TEST_CASE("the package importer reads a truncated .con as if zeros followed and keeps the odd byte of a comment", "[data]")
{
    ConBytes bytes = header(0x1c);
    conversation(bytes, 2);
    event(bytes, 17, "Note");
    bytes.integer(5).raw(std::string("a\0b\0c", 5));
    event(bytes, 0, "Cut");
    bytes.actor(3, "Paul").actor(2, "JCDenton").integer(50).raw("Cut sh");

    dx::compiler::ConFile file;
    std::string error;
    CHECK_FALSE(dx::compiler::readConFile(bytes.data, file, &error));
    CHECK(error == "truncated conversation file");
    REQUIRE(file.conversations.size() == 1);
    REQUIRE(file.conversations[0].events.size() == 2);
    CHECK(file.conversations[0].events[0].comment == "ab");

    Import import("ConQuirkTruncated");
    std::vector<std::string> log = import.run(bytes.data);
    REQUIRE(log.size() == 1);
    CHECK(endsWith(log[0], "/Paul/Talk/Paul01.mp3"));
    std::vector<dx::UObject*> list = importedEvents(import.prefix);
    REQUIRE(list.size() == 2);
    CHECK(text(list[0], "commentText") == "abc");
    CHECK(text(list[1], "Label") == "Cut");
    CHECK(text(reference(list[1], "ConSpeech"), "speech") == "Cut sh");
    CHECK(std::filesystem::exists(import.root / "System" / "ConQuirkTruncatedText.u"));
}
