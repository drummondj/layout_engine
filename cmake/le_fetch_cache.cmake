# Fetched dependencies' sources - never their builds - shared between `le`
# projects. `le` passes LE_SOURCE_CACHE_DIR: a dependency declared with
# le_fetch_declare() whose source is already there (in <name>-<key>, the key
# hashing its declared URL/tag) is used in place instead of downloaded, and
# le_write_fetched_sources() lists every one for `le` to add the newly
# downloaded ones. Builds and subbuilds stay under this build's own
# FETCHCONTENT_BASE_DIR. Without LE_SOURCE_CACHE_DIR, le_fetch_declare() is
# FetchContent_Declare().

set(LE_SOURCE_CACHE_DIR "" CACHE PATH "Shared cache of fetched dependency sources (set by le)")

# FetchContent_Declare(<name> ...), using the cached source if there is one.
# A macro: FETCHCONTENT_SOURCE_DIR_<NAME> must be visible where the
# dependency is made available. A source this build already downloaded
# stays in use, so switching to the cache never reconfigures a dependency.
macro(le_fetch_declare name)
    FetchContent_Declare(${name} ${ARGN})
    if(LE_SOURCE_CACHE_DIR)
        string(SHA256 _le_fetch_key "${ARGN}")
        string(SUBSTRING "${_le_fetch_key}" 0 16 _le_fetch_key)
        set(_le_fetch_entry "${name}-${_le_fetch_key}")
        string(TOLOWER "${name}" _le_fetch_lower)
        string(TOUPPER "${name}" _le_fetch_upper)
        if(IS_DIRECTORY "${LE_SOURCE_CACHE_DIR}/${_le_fetch_entry}"
           AND NOT IS_DIRECTORY "${FETCHCONTENT_BASE_DIR}/${_le_fetch_lower}-src")
            set(FETCHCONTENT_SOURCE_DIR_${_le_fetch_upper} "${LE_SOURCE_CACHE_DIR}/${_le_fetch_entry}")
        endif()
        set_property(GLOBAL APPEND PROPERTY LE_FETCHED_SOURCES "${name}" "${_le_fetch_entry}")
    endif()
endmacro()

# Writes ${PROJECT_BINARY_DIR}/le_fetched_sources.json: each populated
# le_fetch_declare() dependency's name, cache entry and source directory.
function(le_write_fetched_sources)
    if(NOT LE_SOURCE_CACHE_DIR)
        return()
    endif()
    get_property(entries GLOBAL PROPERTY LE_FETCHED_SOURCES)
    set(json "[\n")
    set(separator "")
    while(entries)
        list(POP_FRONT entries name entry)
        string(TOLOWER "${name}" lower)
        FetchContent_GetProperties(${name})
        if(NOT ${lower}_POPULATED)
            continue() # found with find_package instead
        endif()
        set(source "${${lower}_SOURCE_DIR}")
        foreach(var name entry source)
            string(REPLACE "\\" "\\\\" ${var} "${${var}}")
            string(REPLACE "\"" "\\\"" ${var} "${${var}}")
        endforeach()
        string(APPEND json "${separator}  {\"name\": \"${name}\", \"entry\": \"${entry}\", \"source_dir\": \"${source}\"}")
        set(separator ",\n")
    endwhile()
    string(APPEND json "\n]\n")
    file(WRITE "${PROJECT_BINARY_DIR}/le_fetched_sources.json" "${json}")
endfunction()
