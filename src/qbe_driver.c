// Driver program for QBE-compiled Beancraft programs
// This is linked with qbe_runtime.c and the QBE-generated code

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// Functions provided by QBE-generated code
extern uint64_t bc_run(uint64_t max_steps);
extern void bc_set_reg(uint64_t idx, uint64_t value);
extern uint64_t bc_get_reg(uint64_t idx);
extern uint64_t bc_reg_count(void);

// Functions provided by runtime
extern char* bc_bignum_to_string(uint64_t x);

// External register storage and names (defined in QBE code)
extern uint64_t bc_regs[];
extern const char *bc_reg_names[];

// Find register index by name, returns -1 if not found
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

    // Initialize all registers to tagged 0
    uint64_t reg_count = bc_reg_count();
    for (uint64_t i = 0; i < reg_count; i++) {
        bc_set_reg(i, 0);
    }

    // Parse command line arguments for register values
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            verbose = 1;
            continue;
        }

        char *eq = strchr(arg, '=');
        if (eq) {
            *eq = '\0';
            const char *reg_name = arg;
            uint64_t value = (uint64_t)atoll(eq + 1);

            // Try to find by name first
            int64_t idx = find_reg(reg_name, reg_count);

            // If not found by name, try as numeric index
            if (idx < 0) {
                char *endptr;
                uint64_t num_idx = (uint64_t)strtoll(reg_name, &endptr, 10);
                if (*endptr == '\0' && num_idx < reg_count) {
                    idx = (int64_t)num_idx;
                }
            }

            if (idx >= 0) {
                bc_set_reg((uint64_t)idx, value);
                if (verbose) {
                    printf("Set %s = %lu\n", bc_reg_names[idx], value);
                }
            } else {
                fprintf(stderr, "Warning: unknown register '%s'\n", reg_name);
            }
        }
    }

    // Run the program
    if (verbose) {
        printf("Running (max %lu steps)...\n", max_steps);
    }
    uint64_t steps = bc_run(max_steps);

    // Print results
    printf("Results:\n");
    for (uint64_t i = 0; i < reg_count; i++) {
        const char *name = bc_reg_names[i];
        // Skip internal registers (starting with :)
        if (name[0] == ':') continue;

        uint64_t val = bc_get_reg(i);
        printf("%s = %lu\n", name, val);
    }

    if (verbose) {
        printf("\nCompleted in %lu steps\n", steps);
    }

    return 0;
}
