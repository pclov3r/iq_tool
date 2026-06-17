#include <stdio.h>
#include <stdint.h>
#include <string.h>

/*
 * ============================================================================
 * RDS FEC (Forward Error Correction) Lookup Table Generator
 * ============================================================================
 *
 * This utility generates the static O(1) lookup table utilized by the RDS
 * demodulator to perform forward error correction (FEC).
 *
 * Instead of calculating polynomial bit-shifts in real-time for every
 * audio sample, the SDR engine utilizes a precomputed array. This script
 * brute-forces all possible burst errors up to 5 bits in length, calculates
 * their corresponding 10-bit syndromes, and outputs the resulting C array.
 *
 * Usage:
 *     gcc generate_rds_fec_table.c -o gen_lut
 *     ./gen_lut > ../external/libredsea/rds_demodulator_fec_table.h
 */

/*
 * calculate_syndrome()
 *
 * Multiplies a 26-bit input block against the official parity-check matrix
 * defined in the IEC 62106 standard. Returns the resulting 10-bit syndrome.
 */
static uint32_t calculate_syndrome(uint32_t input_vector) {
    // Official 26-bit parity-check matrix for the RDS standard.
    static const uint32_t parity_check_matrix[26] = {
        0x200, 0x100, 0x080, 0x040, 0x020, 
        0x010, 0x008, 0x004, 0x002, 0x001,
        0x2DC, 0x16E, 0x0B7, 0x287, 
        0x39F, 0x313, 0x355, 0x376, 
        0x1BB, 0x201, 0x3DC, 0x1EE, 
        0x0F7, 0x2A7, 0x38F, 0x31B
    };
    
    uint32_t result = 0;
    for (int k = 0; k < 26; k++) {
        // Galois Field GF(2) matrix multiplication
        result ^= (parity_check_matrix[25 - k] * ((input_vector >> k) & 1U));
    }
    return result;
}

int main() {
    /*
     * The RDS specification defines 5 offset words (A, B, C, C', D) which
     * are XOR'd into the syndrome to identify the current block type.
     * A separate lookup table is generated for each offset.
     */
    uint32_t offsets[5] = {0x0FC, 0x198, 0x168, 0x350, 0x1B4};
    const char* offset_names[5] = {"A", "B", "C", "C'", "D"};
    
    // table[offset_idx][syndrome] = error_vector
    static uint32_t fec_table[5][1024];
    memset(fec_table, 0, sizeof(fec_table));
    
    /*
     * Syndrome Aliasing Resolution:
     * A 10-bit syndrome allows for 1,024 unique values. However, there are
     * significantly more than 1,024 possible burst errors across a 26-bit block,
     * resulting in syndrome collisions (aliasing).
     *
     * To resolve these collisions, the algorithm assumes that shorter burst
     * errors are statistically more probable in a typical RF environment.
     * The outer loop processes burst lengths in ascending order (1 to 5).
     * If a syndrome collision occurs, the matrix retains the existing entry,
     * ensuring the table always defaults to the shortest possible burst error.
     */
    for (int length = 1; length <= 5; length++) {
        
        // Traverse the burst window across the 26-bit block
        for (int shift = 0; shift <= 26 - length; shift++) {
            
            // Generate all internal bit combinations for the current length.
            // The boundary bits must remain 1 to ensure strict length compliance.
            int internal_bits = length > 2 ? length - 2 : 0;
            int num_patterns = 1 << internal_bits;
            
            for (int p = 0; p < num_patterns; p++) {
                uint32_t pattern = 1; // start bit
                if (length > 1) {
                    pattern |= (p << 1); // internal bits
                    pattern |= (1 << (length - 1)); // end bit
                }
                
                // Align the pattern to its absolute position in the block
                uint32_t error_vector = pattern << shift;
                
                // Compute the base mathematical syndrome
                uint32_t base_syndrome = calculate_syndrome(error_vector);
                
                // Evaluate against all 5 RDS offsets
                for (int off = 0; off < 5; off++) {
                    uint32_t final_syndrome = base_syndrome ^ offsets[off];
                    
                    // Store the error vector only if the syndrome slot is unoccupied.
                    // This mechanism inherently prioritizes shorter burst lengths.
                    if (fec_table[off][final_syndrome] == 0) {
                        fec_table[off][final_syndrome] = error_vector;
                    }
                }
            }
        }
    }
    
    // ========================================================================
    // HEADER OUTPUT GENERATION
    // ========================================================================
    
    printf("/**\n");
    printf(" * @file rds_demodulator_fec_table.h\n");
    printf(" * @brief Precomputed Forward Error Correction (FEC) Lookup Table for RDS Demodulation.\n");
    printf(" *\n");
    printf(" * This file contains a statically generated O(1) Lookup Table (LUT) used for\n");
    printf(" * correcting burst errors in the RDS (Radio Data System) baseband stream.\n");
    printf(" *\n");
    printf(" * Aliasing (syndrome collisions) is resolved by prioritizing the shortest possible\n");
    printf(" * error burst pattern during generation.\n");
    printf(" */\n\n");
    printf("#ifndef RDS_DEMODULATOR_FEC_TABLE_H\n");
    printf("#define RDS_DEMODULATOR_FEC_TABLE_H\n\n");
    printf("#include <stdint.h>\n\n");
    
    printf("static const uint32_t rds_fec_lookup_table[5][1024] = {\n");
    for (int off = 0; off < 5; off++) {
        printf("// Offset %s (Syndrome Word: 0x%03X)\n", offset_names[off], offsets[off]);
        printf("    {\n");
        
        for (int row = 0; row < 128; row++) {
            printf("        ");
            for (int col = 0; col < 8; col++) {
                printf("0x%08X", fec_table[off][row * 8 + col]);
                
                if (!(row == 127 && col == 7)) {
                    printf(", ");
                }
            }
            printf("\n");
        }
        
        if (off == 4) {
            printf("    }\n");
        } else {
            printf("    },\n");
        }
    }
    printf("};\n\n");
    printf("#endif // RDS_DEMODULATOR_FEC_TABLE_H\n");
    
    return 0;
}
