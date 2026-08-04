#ifndef PICKUP_RECIPE_OUTPUT_H
#define PICKUP_RECIPE_OUTPUT_H

#include <pickup/detect/capability.h>
#include <pickup/detect/recipe.h>

/*
 * Printing a build recipe as TOML.
 *
 * Shared because two commands answer with one: `resolve` says which toolchain
 * to use, and `show` describes one the caller already named. Both have to
 * describe how to build with it, and in the same shape — a consumer parsing
 * two spellings of the same answer would be reading a difference that is not
 * there.
 */
void recipe_print_toml(capability_lang lang, const link_recipe *recipe);

#endif /* PICKUP_RECIPE_OUTPUT_H */
