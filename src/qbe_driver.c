// Driver program for QBE-compiled Beancraft programs.
// Linked with qbe_runtime.c, src/bignum.c and the QBE-generated code.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// Provided by the QBE-generated code
extern uint64_t bc_run(uint64_t max_steps);
extern uint64_t bc_reg_count(void);
extern uint64_t bc_regs[];           // one Bignum per register
extern const char *bc_reg_names[];

// Provided by the runtime
extern void bc_init_reg(uint64_t *reg, uint64_t value);
extern char *bc_bignum_to_string(uint64_t reg);

// A Bignum holding zero is the tagged value 1 (see beancraft/bignum.h).
#define BC_BIGNUM_ZERO 1ULL

// Find register index by name, or -1 if not found.
static int64_t find_reg(const char *name, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        if (strcmp(bc_reg_names[i], name) == 0) {
            return (int64_t)i;
        }
    }
    return -1;
}

int main(int argc, char *argv[]) {
    uint64_t max_steps = 10000000;
    int verbose = 0;

    // bc_regs lives in zero-initialised storage; turn each slot into a valid
    // Bignum zero before anything touches it.
    uint64_t reg_count = bc_reg_count();
    for (uint64_t i = 0; i < reg_count; i++) {
        bc_regs[i] = BC_BIGNUM_ZERO;
    }

    // Parse command line arguments for register values
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            verbose = 1;
            continue;
        }

        char *eq = strchr(arg, '=');
        if (!eq) {
            fprintf(stderr, "Warning: ignoring argument '%s' (expected REG=VALUE)\n", arg);
            continue;
        }

        *eq = '\0';
        const char *reg_name = arg;
        uint64_t value = (uint64_t)strtoull(eq + 1, NULL, 10);

        // Try to find by name first, then as a numeric index.
        int64_t idx = find_reg(reg_name, reg_count);
        if (idx < 0) {
            char *endptr;
            uint64_t num_idx = (uint64_t)strtoull(reg_name, &endptr, 10);
            if (*reg_name != '\0' && *endptr == '\0' && num_idx < reg_count) {
                idx = (int64_t)num_idx;
            }
        }

        if (idx >= 0) {
            bc_init_reg(&bc_regs[idx], value);
            if (verbose) {
                printf("Set %s = %lu\n", bc_reg_names[idx], value);
            }
        } else {
            fprintf(stderr, "Warning: unknown register '%s'\n", reg_name);
        }
    }

    if (verbose) {
        printf("Running (max %lu steps)...\n", max_steps);
    }
    uint64_t steps = bc_run(max_steps);

    printf("Results:\n");
    for (uint64_t i = 0; i < reg_count; i++) {
        const char *name = bc_reg_names[i];
        if (name[0] == ':') continue;  // skip internal registers like :nil

        char *value = bc_bignum_to_string(bc_regs[i]);
        printf("%s = %s\n", name, value);
        free(value);
    }

    if (verbose) {
        printf("\nCompleted in %lu steps\n", steps);
    }

    return 0;
}
