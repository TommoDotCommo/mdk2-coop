#include "../bridge/route.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static struct route_state start = { 1, 1, 100, {0,0,0}, {1,0,0,0} };
static char error[160];

static FILE *fixture(int end_cp, int footer)
{
    struct route_frame v = {0};
    FILE *f = tmpfile();
    assert(f);
    assert(route_write_header(f, &start));
    v.state = start;
    v.input.keys[17/8] = 1 << (17%8);
    v.input.mouse[0] = 3;
    assert(route_write_frame(f, &v));
    v.ms = 1000;
    v.state.cp = end_cp;
    v.state.pos[1] = 10;
    memset(&v.input, 0, sizeof(v.input));
    assert(route_write_frame(f, &v));
    if (footer) fputs("END 1000\n", f);
    rewind(f);
    return f;
}

static void parser_failures(void)
{
    struct route r;
    FILE *f = fixture(1, 1);
    assert(!route_load(f, &r, error, sizeof(error))); fclose(f);
    f = fixture(2, 0);
    assert(!route_load(f, &r, error, sizeof(error))); fclose(f);
    const char *bad[] = {
        "MDKROUTE 2 3902\n", "MDKROUTE 1 3903\n",
        "MDKROUTE 1 3902\nS 1 1 100 nan 0 0 1 0 0 0\n",
        "MDKROUTE 1 3902\nS 1 1 100 0 0 0 0 0 0 0\n",
    };
    for (unsigned int i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        f = tmpfile(); fputs(bad[i], f); rewind(f);
        assert(!route_load(f, &r, error, sizeof(error))); fclose(f);
    }
    f = fixture(2, 1); fseek(f, 0, SEEK_END); fputs("junk\n", f); rewind(f);
    assert(!route_load(f, &r, error, sizeof(error))); fclose(f);
    /* Mutation twins: forbidden checkpoint keys and backwards time cannot make valid routes. */
    for (int mutation = 0; mutation < 4; mutation++) {
        struct route_frame v = {0};
        f = tmpfile(); route_write_header(f, &start);
        v.state = start; v.ms = 500; route_write_frame(f, &v);
        v.state.cp = 2; v.ms = mutation == 0 ? 100 : 1000;
        if (mutation == 1) v.input.keys[64/8] = 1 << (64%8);
        if (mutation == 2) v.input.mouse[0] = 100001;
        route_write_frame(f, &v);
        fprintf(f, "END %u\n", mutation == 3 ? 999 : v.ms); rewind(f);
        assert(!route_load(f, &r, error, sizeof(error))); fclose(f);
    }
}

int main(void)
{
    struct route r;
    struct route_player p;
    struct route_input input;
    struct route_state s;
    FILE *f = fixture(2, 1);
    assert(route_load(f, &r, error, sizeof(error))); fclose(f);
    assert(route_begin(&p, &r, &start, error, sizeof(error)));
    route_input_at(&p, 0, &input);
    assert(input.keys[17/8] & (1 << (17%8)));
    assert(input.mouse[0] == 3);
    route_input_at(&p, 20, &input);
    assert(input.mouse[0] == 0); /* never replay a mouse delta twice */
    assert(input.keys[17/8] & (1 << (17%8)));
    s = start;
    assert(route_check(&p, 20, &s, error, sizeof(error)) == 0);
    s.cp = 2; s.pos[1] = 10;
    assert(route_check(&p, 950, &s, error, sizeof(error)) == 0);
    route_input_at(&p, 1000, &input);
    for (int i = 0; i < ROUTE_BYTES; i++) assert(input.keys[i] == 0);
    assert(route_check(&p, 1000, &s, error, sizeof(error)) == 1);
    s = start; s.pos[0] = 1;
    assert(!route_begin(&p, &r, &s, error, sizeof(error)));
    s = start; s.rot[0] = 0; s.rot[3] = 1;
    assert(!route_begin(&p, &r, &s, error, sizeof(error)));
    assert(route_begin(&p, &r, &start, error, sizeof(error)));
    assert(route_check(&p, 6001, &start, error, sizeof(error)) == -1); /* stalled */
    assert(route_begin(&p, &r, &start, error, sizeof(error)));
    s = start; s.cp = 3;
    assert(route_check(&p, 1000, &s, error, sizeof(error)) == -1); /* wrong progression */
    assert(route_begin(&p, &r, &start, error, sizeof(error)));
    s = start; s.cp = 2; s.pos[0] = 100;
    route_input_at(&p, 1000, &input);
    assert(route_check(&p, 1000, &s, error, sizeof(error)) == 0);
    assert(route_check(&p, 3100, &s, error, sizeof(error)) == -1); /* diverged */
    route_free(&r);
    /* Reaching the final checkpoint early cannot hide missing intermediate milestones. */
    f = tmpfile();
    route_write_header(f, &start);
    struct route_frame cycle = {0};
    cycle.state = start; route_write_frame(f, &cycle);
    cycle.ms = 1000; cycle.state.cp = 2; route_write_frame(f, &cycle);
    cycle.ms = 2000; cycle.state.cp = 3; route_write_frame(f, &cycle);
    cycle.ms = 3000; cycle.state.cp = 2; route_write_frame(f, &cycle);
    fputs("END 3000\n", f); rewind(f);
    assert(route_load(f, &r, error, sizeof(error))); fclose(f);
    assert(route_begin(&p, &r, &start, error, sizeof(error)));
    s = start; s.cp = 2;
    assert(route_check(&p, 1000, &s, error, sizeof(error)) == 0);
    assert(route_check(&p, 3000, &s, error, sizeof(error)) == 0);
    s.cp = 3;
    assert(route_check(&p, 3100, &s, error, sizeof(error)) == 0);
    s.cp = 2;
    assert(route_check(&p, 3200, &s, error, sizeof(error)) == 1);
    assert(route_begin(&p, &r, &start, error, sizeof(error)));
    assert(route_check(&p, 6001, &s, error, sizeof(error)) == -1);
    route_free(&r);
    parser_failures();
    puts("route: parser, input playback, progression, endpoint and failure twins PASS");
    return 0;
}
