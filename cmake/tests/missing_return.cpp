// A non-void function that falls off its end on one path: extension code
// compiled with LE_EXTENSION_COMPILE_OPTIONS must fail to build.
bool bump(int value)
{
    if (value < 0)
        return false;
}
