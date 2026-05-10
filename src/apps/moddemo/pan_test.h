/* Quick stereo pan test — call after of_mixer_init and sample load */
static void pan_test(void) {
    /* Use the first sample in the MOD */
    if (!mod->sample_data[0]) return;
    mod_sample_t *s = &mod->samples[0];

    printf("Pan test: L then R then both\n");

    /* Left only */
    int vl = of_mixer_play((const uint8_t *)mod->sample_data[0],
                           s->length, 8363, 0, 200);
    if (vl >= 0) {
        of_mixer_set_pan(vl, 0);
        if (s->loop_length > 2)
            of_mixer_set_loop(vl, s->loop_start, s->loop_start + s->loop_length);
    }
    printf(" L voice=%d\n", vl);
    usleep(1000 * 1000);
    if (vl >= 0) of_mixer_stop(vl);
    usleep(200 * 1000);

    /* Right only */
    int vr = of_mixer_play((const uint8_t *)mod->sample_data[0],
                           s->length, 8363, 0, 200);
    if (vr >= 0) {
        of_mixer_set_pan(vr, 255);
        if (s->loop_length > 2)
            of_mixer_set_loop(vr, s->loop_start, s->loop_start + s->loop_length);
    }
    printf(" R voice=%d\n", vr);
    usleep(1000 * 1000);
    if (vr >= 0) of_mixer_stop(vr);
    usleep(200 * 1000);

    /* Both simultaneously */
    vl = of_mixer_play((const uint8_t *)mod->sample_data[0],
                       s->length, 8363, 0, 200);
    vr = of_mixer_play((const uint8_t *)mod->sample_data[0],
                       s->length, 8363, 0, 200);
    if (vl >= 0) {
        of_mixer_set_pan(vl, 0);
        if (s->loop_length > 2)
            of_mixer_set_loop(vl, s->loop_start, s->loop_start + s->loop_length);
    }
    if (vr >= 0) {
        of_mixer_set_pan(vr, 255);
        if (s->loop_length > 2)
            of_mixer_set_loop(vr, s->loop_start, s->loop_start + s->loop_length);
    }
    printf(" L=%d R=%d (both)\n", vl, vr);
    usleep(2000 * 1000);
    if (vl >= 0) of_mixer_stop(vl);
    if (vr >= 0) of_mixer_stop(vr);

    printf("Pan test done\n");
}
