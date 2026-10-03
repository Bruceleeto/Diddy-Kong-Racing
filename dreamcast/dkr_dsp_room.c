#include "dkr_dsp_room.h"

/* asset_audio_9.bin: AL_FX_CUSTOM's 6,240-sample N64 delay line, translated
   to AICA's 44.1 kHz clock. Coefficients are rounded to AICA's 8-LSB grid. */
enum {
    N64_FF = -9832, N64_FB = 9832,
    N64_LONG_FF = -16384, N64_LONG_FB = 16384,
    N64_RETURN_1 = 11144, N64_RETURN_2 = 4584,
    TAP_160 = 320, TAP_320 = 640, TAP_800 = 1600, TAP_2560 = 5120,
    TAP_3200 = 6400, TAP_5600 = 11200, TAP_5920 = 11840,
    RAW_0 = 70, RAW_1 = 72, RAW_2 = 74, RAW_3 = 76, RAW_4 = 78,
};

typedef struct { uint8_t xsel, ira, tra; } source_t;

static int put(afx_dsp_program_t *program, uint8_t step, afx_dsp_step_t value) {
    if (!value.ysel) value.ysel = 1;
    return afx_dsp_program_step(program, step, &value);
}

static int coefficient(afx_dsp_program_t *program, uint8_t step, int16_t value) {
    return afx_dsp_program_coefficient(program, step, value);
}

static int address(afx_dsp_program_t *program, uint8_t masa, uint16_t value) {
    return afx_dsp_program_address(program, masa * 2, value);
}

static afx_dsp_step_t from(source_t source) {
    return (afx_dsp_step_t){.xsel = source.xsel, .ira = source.ira, .tra = source.tra};
}

static int set_coefficient(afx_dsp_program_t *program, uint8_t step,
                           afx_dsp_step_t value, int16_t gain) {
    int result = put(program, step, value);
    return result ? result : coefficient(program, step, gain);
}

/* Read one signed-linear delay tap into a MEMS latch.  The two intervening
   instruction slots are intentional: the AICA makes MRD visible at IWT + 2. */
static int read_tap(afx_dsp_program_t *program, uint8_t step, uint8_t masa, uint8_t mems) {
    int result = put(program, step + 1,
                     (afx_dsp_step_t){.mrd = 1, .nofl = 1, .masa = masa, .zero = 1});
    if (!result) result = put(program, step + 3, (afx_dsp_step_t){.iwt = 1, .iwa = mems, .zero = 1});
    return result;
}

/* One N64 delay section with an already available input. The delayed cell is
   read first; raw = delayed + ff * input. Feedback replaces the input cell and
   the filtered raw replaces the output cell. TWT/MWT consume the previous
   ACC, so step +6 saves raw while starting the filter, and step +9 writes the
   preceding filtered value while starting the feedback sum. */
static int section(afx_dsp_program_t *program, uint8_t step, source_t input,
                   uint8_t out_read_masa, uint8_t out_write_masa, uint8_t input_write_masa,
                   uint8_t mems, uint8_t raw, uint8_t state_read, uint8_t state_write,
                   int16_t feedback, int16_t feedforward, int16_t lowpass) {
    int result = read_tap(program, step, out_read_masa, mems);
    if (!result) result = set_coefficient(program, step + 4,
        (afx_dsp_step_t){.ira = mems, .xsel = 1, .zero = 1}, 32760);
    afx_dsp_step_t mixed = from(input);
    mixed.bsel = 1;
    if (!result) result = set_coefficient(program, step + 5, mixed, feedforward);
    /* Save raw before using the rotating TEMP pole state. */
    afx_dsp_step_t state = {.tra = state_read, .twt = 1, .twa = raw, .zero = 1};
    if (!result) result = set_coefficient(program, step + 6, state, lowpass);
    afx_dsp_step_t filtered = {.tra = raw, .bsel = 1};
    if (!result) result = set_coefficient(program, step + 7, filtered, (int16_t)(32760 - lowpass));
    /* Write the pole state and retain filtered raw for the delayed output write. */
    if (!result) result = put(program, step + 8,
        (afx_dsp_step_t){.twt = 1, .twa = state_write, .bsel = 1});
    afx_dsp_step_t feedback_value = {.tra = raw, .mwt = 1, .nofl = 1,
                                     .masa = out_write_masa, .zero = 1};
    if (!result) result = set_coefficient(program, step + 9, feedback_value, feedback);
    afx_dsp_step_t input_sum = from(input);
    input_sum.bsel = 1;
    if (!result) result = set_coefficient(program, step + 10, input_sum, 32760);
    if (!result) result = put(program, step + 11,
        (afx_dsp_step_t){.mwt = 1, .nofl = 1, .masa = input_write_masa, .bsel = 1});
    return result;
}

/* Sections two and three read both the shared-line input and output taps. */
static int delayed_section(afx_dsp_program_t *program, uint8_t step,
                           uint8_t out_read_masa, uint8_t input_read_masa,
                           uint8_t out_write_masa, uint8_t input_write_masa,
                           uint8_t out_mems, uint8_t input_mems, uint8_t raw,
                           uint8_t state_read, uint8_t state_write,
                           int16_t feedback, int16_t feedforward, int16_t lowpass) {
    int result = read_tap(program, step, out_read_masa, out_mems);
    if (!result) result = read_tap(program, step + 4, input_read_masa, input_mems);
    if (!result) result = set_coefficient(program, step + 8,
        (afx_dsp_step_t){.ira = out_mems, .xsel = 1, .zero = 1}, 32760);
    if (!result) result = set_coefficient(program, step + 9,
        (afx_dsp_step_t){.ira = input_mems, .xsel = 1, .bsel = 1}, feedforward);
    if (!result) result = set_coefficient(program, step + 10,
        (afx_dsp_step_t){.tra = state_read, .twt = 1, .twa = raw, .zero = 1}, lowpass);
    if (!result) result = set_coefficient(program, step + 11,
        (afx_dsp_step_t){.tra = raw, .bsel = 1}, (int16_t)(32760 - lowpass));
    if (!result) result = put(program, step + 12,
        (afx_dsp_step_t){.twt = 1, .twa = state_write, .bsel = 1});
    if (!result) result = set_coefficient(program, step + 13,
        (afx_dsp_step_t){.tra = raw, .mwt = 1, .nofl = 1,
                         .masa = out_write_masa, .zero = 1}, feedback);
    if (!result) result = set_coefficient(program, step + 14,
        (afx_dsp_step_t){.ira = input_mems, .xsel = 1, .bsel = 1}, 32760);
    if (!result) result = put(program, step + 15,
        (afx_dsp_step_t){.mwt = 1, .nofl = 1, .masa = input_write_masa, .bsel = 1});
    return result;
}

int dkr_dsp_program_n64_room_sections(afx_dsp_program_t *program, uint8_t sections) {
    if (!program || sections < 1 || sections > 5) return -AFX_BAD_COMMAND;
    int result = afx_dsp_program_init(program);

    /* 0: 0/160 all-pass. Its raw result directly feeds section 1. */
    if (!result) result = section(program, 0, (source_t){.xsel = 1, .ira = 32},
                                   0, 1, 2, 0, RAW_0, 0, 127, N64_FB, N64_FF, 0);
    /* 1: 160/320, first audible branch. */
    if (!result && sections >= 2) result = section(program, 12, (source_t){.tra = RAW_0},
                                   3, 4, 5, 1, RAW_1, 2, 1, N64_FB, N64_FF, 9472);
    /* 2: 800/2560 and 3: 3200/5600 read independent shared-line taps. */
    if (!result && sections >= 3) result = delayed_section(program, 24, 6, 7, 8, 9, 2, 3, RAW_2, 4, 3,
                                           N64_LONG_FB, N64_LONG_FF, 12288);
    if (!result && sections >= 4) result = delayed_section(program, 40, 10, 11, 12, 13, 4, 5, RAW_3, 6, 5,
                                           N64_LONG_FB, N64_LONG_FF, 13568);
    /* The final, zero-return N64 section feeds back into the first input. Its
       slow chorus resampler (rate 379, depth 10) is added only after this
       static network matches. */
    if (!result && sections >= 5) result = delayed_section(program, 56, 14, 15, 16, 17, 6, 7, RAW_4, 8, 7,
                                           13000, -13000, 17664);

    /* N64 returns the same mono wet bus on its two auxiliary channels. */
    if (!result && sections >= 2) result = set_coefficient(program, 72,
        (afx_dsp_step_t){.tra = 1, .zero = 1}, N64_RETURN_1);
    if (!result && sections >= 3) result = set_coefficient(program, 73,
        (afx_dsp_step_t){.tra = 3, .bsel = 1}, N64_RETURN_2);
    else if (!result && sections >= 2) result = put(program, 73, (afx_dsp_step_t){.bsel = 1});
    if (!result && sections >= 4) result = set_coefficient(program, 74,
        (afx_dsp_step_t){.tra = 5, .bsel = 1}, N64_RETURN_2);
    else if (!result && sections >= 2) result = put(program, 74, (afx_dsp_step_t){.bsel = 1});
    if (!result && sections >= 2) result = put(program, 75, (afx_dsp_step_t){.ewt = 1, .ewa = 0, .bsel = 1});
    if (!result && sections >= 2) result = put(program, 76, (afx_dsp_step_t){.ewt = 1, .ewa = 1, .bsel = 1});

    /* One shared AICA ring, in doubled 44.1 kHz sample offsets. Separate
       MADRS entries deliberately alias a cell when N64 reads and writes it. */
    static const uint16_t offsets[] = {
        TAP_160, TAP_160, 0,
        TAP_320, TAP_320, TAP_160,
        TAP_2560, TAP_800, TAP_2560, TAP_800,
        TAP_5600, TAP_3200, TAP_5600, TAP_3200,
        TAP_5920, 0, TAP_5920, 0,
    };
    for (uint8_t masa = 0; !result && masa < sizeof(offsets) / sizeof(*offsets); ++masa)
        result = address(program, masa, offsets[masa]);
    return result;
}

int dkr_dsp_program_n64_room(afx_dsp_program_t *program) {
    return dkr_dsp_program_n64_room_sections(program, 5);
}
