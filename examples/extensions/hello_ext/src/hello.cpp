#include "hello_ext/hello.hpp"

static_assert(LE_EXTENSION_API_VERSION == 1, "hello_ext targets extension API v1");

namespace hello
{
    int library_count(le::ext::ExtensionContext &ctx)
    {
        const le::ext::ReadView view = ctx.read();
        return static_cast<int>(view.root().get_library_ids().size());
    }

    bool add_library(le::ext::ExtensionContext &ctx, const std::string &name)
    {
        le::ext::Transaction transaction = ctx.transaction("hello_add_library " + name);
        if (name.empty())
        {
            transaction.fail();
            return false;
        }
        const LeLibraryId library = le_create_library(ctx.handle(), ("hello_" + name).c_str());
        if (library.index == UINT32_MAX)
        {
            transaction.fail();
            return false;
        }
        ++ctx.data<State>().libraries_added;
        return true;
    }
}

namespace hello
{
    std::vector<std::string> notes_on(le::ext::ExtensionContext &ctx, const std::string &library)
    {
        const le::ext::ReadView view = ctx.read();
        const le::Root &root = view.root();
        std::vector<std::string> texts;
        const le::LibraryId id = root.get_library_by_name(library);
        if (!root.get_library(id))
            return texts;
        for (const le::HelloNoteId note : root.get_library_hello_notes(id))
            texts.push_back(root.get_hello_note(note)->body);
        return texts;
    }
}

namespace
{
    // hello_ext's section of settings.json: {"version": 1, "library_name": "..."}.
    nlohmann::json save_settings(le::ext::ExtensionContext &ctx)
    {
        return {{"library_name", ctx.data<hello::State>().library_name}};
    }

    void load_settings(le::ext::ExtensionContext &ctx, const nlohmann::json &section, int /*version*/)
    {
        if (section.contains("library_name") && section["library_name"].is_string())
            ctx.data<hello::State>().library_name = section["library_name"].get<std::string>();
    }
}

void le_ext_hello_ext_register(le::ext::Registry &registry)
{
    registry.add_settings({.version = 1, .save = save_settings, .load = load_settings});
}
