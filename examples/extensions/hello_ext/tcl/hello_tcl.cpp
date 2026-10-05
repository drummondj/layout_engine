#include "hello_ext/hello.hpp"
#include "hello_ext/hello_tcl.hpp"

#include <le/extension_tcl.hpp>

namespace
{
    le::ext::ExtensionContext context()
    {
        return le::ext::ExtensionContext(le::ext::tcl_session(), "hello_ext");
    }

    // hello_ext_info: a command made with the raw Tcl API (TCL_INIT), for
    // what SWIG doesn't fit.
    int info_command(ClientData, Tcl_Interp *interp, int, Tcl_Obj *const[])
    {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("hello_ext 0.1.0", -1));
        return TCL_OK;
    }
}

int hello_library_count_cmd()
{
    le::ext::ExtensionContext ctx = context();
    return hello::library_count(ctx);
}

int hello_add_greeting_cmd(const char *name)
{
    le::ext::ExtensionContext ctx = context();
    return hello::add_greeting(ctx, name ? name : "") ? 0 : 1;
}

int hello_greetings_added_cmd()
{
    return context().data<hello::State>().greetings_added;
}

void le_ext_hello_ext_init_tcl(Tcl_Interp *interp)
{
    Tcl_CreateObjCommand(interp, "hello_ext_info", info_command, nullptr, nullptr);
}
