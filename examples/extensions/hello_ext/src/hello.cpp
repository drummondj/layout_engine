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

void le_ext_hello_ext_register(le::ext::Registry &)
{
}
