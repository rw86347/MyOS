#ifndef LINUX_KCONFIG_H
#define LINUX_KCONFIG_H

/*
 * IS_ENABLED(CONFIG_FOO): 1 if the build defines CONFIG_FOO to 1 (or
 * CONFIG_FOO_MODULE), else 0 — usable in plain C `if`s, so both branches
 * are compiled and type-checked. The trick, as in Linux: pasting a defined
 * option's value 1 onto __ARG_PLACEHOLDER_ makes "0," — an extra argument
 * that shifts the 1 into the position __take_second_arg returns; an
 * undefined option pastes into junk, and the 0 stays second.
 * The CONFIG_* a driver is built with are set where it is compiled.
 */
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define __is_defined(x)              ___is_defined(x)
#define ___is_defined(val)           ____is_defined(__ARG_PLACEHOLDER_##val)
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define IS_BUILTIN(option)   __is_defined(option)
#define IS_MODULE(option)    __is_defined(option##_MODULE)
#define IS_ENABLED(option)   (IS_BUILTIN(option) || IS_MODULE(option))
#define IS_REACHABLE(option) IS_ENABLED(option)

#endif
