#include "ruby/ruby.h"
#include "ruby/debug.h"

struct tracepoint_track {
    size_t newobj_count;
    size_t free_count;
    size_t gc_start_count;
    size_t gc_end_count;
    size_t objects_count;
    VALUE objects[10];
};

#define objects_max (sizeof(((struct tracepoint_track *)NULL)->objects)/sizeof(VALUE))

static void
tracepoint_track_objspace_events_i(VALUE tpval, void *data)
{
    rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tpval);
    struct tracepoint_track *track = data;

    switch (rb_tracearg_event_flag(tparg)) {
      case RUBY_INTERNAL_EVENT_NEWOBJ:
	{
	    VALUE obj = rb_tracearg_object(tparg);
	    if (track->objects_count < objects_max)
		track->objects[track->objects_count++] = obj;
	    track->newobj_count++;
	    break;
	}
      case RUBY_INTERNAL_EVENT_FREEOBJ:
	{
	    track->free_count++;
	    break;
	}
      case RUBY_INTERNAL_EVENT_GC_START:
	{
	    track->gc_start_count++;
	    break;
	}
      case RUBY_INTERNAL_EVENT_GC_END:
	{
	    track->gc_end_count++;
	    break;
	}
      default:
	rb_raise(rb_eRuntimeError, "unknown event");
    }
}

static VALUE
tracepoint_track_objspace_events(VALUE self)
{
    struct tracepoint_track track = {0, 0, 0, 0, 0,};
    VALUE tpval = rb_tracepoint_new(0, RUBY_INTERNAL_EVENT_NEWOBJ | RUBY_INTERNAL_EVENT_FREEOBJ |
				    RUBY_INTERNAL_EVENT_GC_START | RUBY_INTERNAL_EVENT_GC_END,
				    tracepoint_track_objspace_events_i, &track);
    VALUE result = rb_ary_new();

    rb_tracepoint_enable(tpval);
    rb_yield(Qundef);
    rb_tracepoint_disable(tpval);

    rb_ary_push(result, SIZET2NUM(track.newobj_count));
    rb_ary_push(result, SIZET2NUM(track.free_count));
    rb_ary_push(result, SIZET2NUM(track.gc_start_count));
    rb_ary_push(result, SIZET2NUM(track.gc_end_count));
    rb_ary_cat(result, track.objects, track.objects_count);

    return result;
}

void
Init_tracepoint(void)
{
    VALUE mBug = rb_define_module("Bug");
    rb_define_module_function(mBug, "tracepoint_track_objspace_events", tracepoint_track_objspace_events, 0);
}
