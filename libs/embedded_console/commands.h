#ifndef COMMANDS_H
#define COMMANDS_H

#include <stddef.h>

/**
 * @brief Splits a line on spaces and runs the matching command.
 *
 * The line is modified in place (tokenised), so pass a buffer you own.
 * Unknown commands print an error; an empty line does nothing.
 *
 * @param line Null-terminated command line.
 */
void commands_execute(char* line);

/**
 * @brief Finds registered commands whose name starts with a prefix.
 *
 * @param prefix  Null-terminated text typed so far (may be empty).
 * @param matches Output array of command names (pointers into the registry, not copies).
 * @param max     Capacity of matches.
 * @return Number of entries written to matches.
 */
size_t commands_complete(const char* prefix, const char** matches, size_t max);

#endif
