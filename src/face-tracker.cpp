#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include "plugin-macros.generated.h"
#include "face-detector-base.h"
#include "face-detector-dlib.h"
#include "face-tracker-base.h"
#include "face-tracker-dlib.h"
#include <algorithm>
#include <deque>
#include <math.h>
#include <graphics/matrix4.h>

struct rectf_s
{
	float x0;
	float y0;
	float x1;
	float y1;
};

struct f3
{
	float v[3];

	f3 (const f3 &a) {*this=a;}
	f3 (float a, float b, float c) { v[0]=a; v[1]=b; v[2]=c; }
	f3 (const rect_s &a) { v[0]=(a.x0+a.x1)*0.5f; v[1]=(a.y0+a.y1)*0.5f; v[2]=sqrtf((a.x1-a.x0)*(a.y1-a.y0)); }
	f3 (const rectf_s &a) { v[0]=(a.x0+a.x1)*0.5f; v[1]=(a.y0+a.y1)*0.5f; v[2]=sqrtf((a.x1-a.x0)*(a.y1-a.y0)); }
	f3 operator + (const f3 &a) { return f3 (v[0]+a.v[0], v[1]+a.v[1], v[2]+a.v[2]); }
	f3 operator - (const f3 &a) { return f3 (v[0]-a.v[0], v[1]-a.v[1], v[2]-a.v[2]); }
	f3 operator * (float a) { return f3 (v[0]*a, v[1]*a, v[2]*a); }
	f3 & operator += (const f3 &a) { return *this = f3 (v[0]+a.v[0], v[1]+a.v[1], v[2]+a.v[2]); }
};

static inline int get_width (const rect_s &r) { return r.x1 - r.x0; }
static inline int get_height(const rect_s &r) { return r.y1 - r.y0; }
static inline int get_width (const rectf_s &r) { return r.x1 - r.x0; }
static inline int get_height(const rectf_s &r) { return r.y1 - r.y0; }

static inline float common_length(float a0, float a1, float b0, float b1)
{
	// assumes a0 < a1, b0 < b1
	// if (a1 <= b0) return 0.0f; // a0 < a1 < b0 < b1
	if (a0 <= b0 && b0 <= a1 && a1 <= b1) return a1 - b0; // a0 < b0 < a1 < b1
	if (a0 <= b0 && b1 <= a1) return b1 - b0; // a0 < b0 < b1 < a1
	if (b0 <= a0 && a1 <= b1) return a1 - a0; // b0 < a0 < a1 < b1
	if (b0 <= a0 && a0 <= b1 && a0 <= b1) return b1 - a0; // b0 < a0 < b1 < a1
	// if (b1 <= a0) return 0.0f; // b0 < b1 < a0 < a1
	return 0.0f;
}

static inline float common_area(const rect_s &a, const rect_s &b)
{
	return common_length(a.x0, a.x1, b.x0, b.x1) * common_length(a.y0, a.y1, b.y0, b.y1);
}

struct tracker_inst_s
{
	face_tracker_base *tracker;
	rect_s rect;
	rectf_s crop_tracker; // crop corresponding to current processing image
	rectf_s crop_rect; // crop corresponding to rect
	float att;
	enum tracker_state_e {
		tracker_state_init = 0,
		tracker_state_reset_texture, // texture has been set, position is not set.
		tracker_state_constructing, // texture and positions have been set, starting to construct correlation_tracker.
		tracker_state_first_track, // correlation_tracker has been prepared, running 1st tracking
		tracker_state_available, // 1st tracking was done, `rect` is available, can accept next frame.
		tracker_state_ending,
	} state;
	int tick_cnt;
};

struct face_tracker_filter
{
	obs_source_t *context;
	gs_texrender_t *texrender;
	gs_stagesurf_t* stagesurface;
	uint32_t known_width;
	uint32_t known_height;
	int tick_cnt;
	int next_tick_stage_to_detector;
	bool target_valid;
	bool rendered;
	bool staged;
	bool is_active;
	bool detector_in_progress;

	face_detector_base *detect;
	std::vector<rect_s> *rects;
	int detect_tick;

	std::deque<struct tracker_inst_s> *trackers;
	std::deque<struct tracker_inst_s> *trackers_idlepool;

	rectf_s crop_cur;
	f3 detect_err;
	f3 range_min, range_max;

	float upsize_l, upsize_r, upsize_t, upsize_b;
	float track_z, track_x, track_y;
	float scale_max;

	float kp;
	float ki;
	float klpf;
	float tlpf;
	f3 e_deadband, e_nonlinear; // deadband and nonlinear amount for error input
	f3 filter_int_out;
	f3 filter_int;
	f3 filter_lpf;

	bool debug_faces;
	bool debug_notrack;
};

static const char *ftf_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Face Tracker");
}

static void ftf_update(void *data, obs_data_t *settings)
{
	auto *s = (struct face_tracker_filter*)data;

	s->upsize_l = obs_data_get_double(settings, "upsize_l");
	s->upsize_r = obs_data_get_double(settings, "upsize_r");
	s->upsize_t = obs_data_get_double(settings, "upsize_t");
	s->upsize_b = obs_data_get_double(settings, "upsize_b");
	s->track_z = obs_data_get_double(settings, "track_z");
	s->track_x = obs_data_get_double(settings, "track_x");
	s->track_y = obs_data_get_double(settings, "track_y");
	s->scale_max = obs_data_get_double(settings, "scale_max");

	double kp = obs_data_get_double(settings, "Kp");
	float ki = (float)obs_data_get_double(settings, "Ki");
	double td = obs_data_get_double(settings, "Td");
	s->kp = (float)kp;
	s->ki = ki;
	s->klpf = (float)(td * kp);
	s->tlpf = (float)obs_data_get_double(settings, "Tdlpf");
	s->e_deadband.v[0] = (float)obs_data_get_double(settings, "e_deadband_x") * 1e-2;
	s->e_deadband.v[1] = (float)obs_data_get_double(settings, "e_deadband_y") * 1e-2;
	s->e_deadband.v[2] = (float)obs_data_get_double(settings, "e_deadband_z") * 1e-2;
	s->e_nonlinear.v[0] = (float)obs_data_get_double(settings, "e_nonlinear_x") * 1e-2;
	s->e_nonlinear.v[1] = (float)obs_data_get_double(settings, "e_nonlinear_y") * 1e-2;
	s->e_nonlinear.v[2] = (float)obs_data_get_double(settings, "e_nonlinear_z") * 1e-2;

	s->debug_faces = obs_data_get_bool(settings, "debug_faces");
	s->debug_notrack = obs_data_get_bool(settings, "debug_notrack");
}

static void *ftf_create(obs_data_t *settings, obs_source_t *context)
{
	auto *s = (struct face_tracker_filter*)bzalloc(sizeof(struct face_tracker_filter));
	s->rects = new std::vector<rect_s>;
	s->crop_cur.x1 = s->crop_cur.y1 = -2;
	s->context = context;
	s->detect = new face_detector_dlib();
	s->detect->start();
	s->trackers = new std::deque<struct tracker_inst_s>;
	s->trackers_idlepool = new std::deque<struct tracker_inst_s>;

	obs_source_update(context, settings);
	return s;
}

static void ftf_destroy(void *data)
{
	auto *s = (struct face_tracker_filter*)data;

	obs_enter_graphics();
	gs_texrender_destroy(s->texrender);
	s->texrender = NULL;
	gs_stagesurface_destroy(s->stagesurface);
	s->stagesurface = NULL;
	obs_leave_graphics();

	s->detect->stop();

	delete s->rects;
	delete s->trackers;
	delete s->trackers_idlepool;
	bfree(s);
}

static bool ftf_reset_tracking(obs_properties_t *, obs_property_t *, void *data)
{
	auto *s = (struct face_tracker_filter*)data;

	float w = s->known_width;
	float h = s->known_height;
	float z = sqrtf(w*h);
	s->detect_err = f3(0, 0, 0);
	s->filter_int_out = f3(w*0.5f, h*0.5f, z);
	s->filter_int = f3(0, 0, 0);
	s->filter_lpf = f3(0, 0, 0);

	return true;
}

static obs_properties_t *ftf_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props;
	props = obs_properties_create();

	obs_properties_add_button(props, "ftf_reset_tracking", obs_module_text("Reset tracking"), ftf_reset_tracking);

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_float(pp, "upsize_l", obs_module_text("Left"), -0.4, 4.0, 0.2);
		obs_properties_add_float(pp, "upsize_r", obs_module_text("Right"), -0.4, 4.0, 0.2);
		obs_properties_add_float(pp, "upsize_t", obs_module_text("Top"), -0.4, 4.0, 0.2);
		obs_properties_add_float(pp, "upsize_b", obs_module_text("Bottom"), -0.4, 4.0, 0.2);
		obs_properties_add_group(props, "upsize", obs_module_text("Upsize recognized face"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_float(pp, "track_z", obs_module_text("Zoom"), 0.1, 2.0, 0.1);
		obs_properties_add_float(pp, "track_x", obs_module_text("X"), -1.0, +1.0, 0.05);
		obs_properties_add_float(pp, "track_y", obs_module_text("Y"), -1.0, +1.0, 0.05);
		obs_properties_add_float(pp, "scale_max", obs_module_text("Scale max"), 1.0, 20.0, 1.0);
		obs_properties_add_group(props, "track", obs_module_text("Tracking target location"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_float(pp, "Kp", "Track Kp", 0.01, 10.0, 0.1);
		obs_properties_add_float(pp, "Ki", "Track Ki", 0.0, 5.0, 0.01);
		obs_properties_add_float(pp, "Td", "Track Td", 0.0, 5.0, 0.01);
		obs_properties_add_float(pp, "Tdlpf", "Track LPF for Td", 0.0, 10.0, 0.1);
		obs_properties_add_float(pp, "e_deadband_x", "Dead band (X)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_deadband_y", "Dead band (Y)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_deadband_z", "Dead band (Z)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_nonlinear_x", "Nonlinear band (X)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_nonlinear_y", "Nonlinear band (Y)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_nonlinear_z", "Nonlinear band (Z)", 0.0, 50, 0.1);
		obs_properties_add_group(props, "ctrl", obs_module_text("Tracking response"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_bool(pp, "debug_faces", "Show face detection results");
		obs_properties_add_bool(pp, "debug_notrack", "Stop tracking faces");
		obs_properties_add_group(props, "debug", obs_module_text("Debugging"), OBS_GROUP_NORMAL, pp);
	}

	return props;
}

static void ftf_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "upsize_l", 0.2);
	obs_data_set_default_double(settings, "upsize_r", 0.2);
	obs_data_set_default_double(settings, "upsize_t", 0.3);
	obs_data_set_default_double(settings, "upsize_b", 0.1);
	obs_data_set_default_double(settings, "track_z",  0.70); //  1.00  0.50  0.35
	obs_data_set_default_double(settings, "track_y", +0.00); // +0.00 +0.10 +0.30
	obs_data_set_default_double(settings, "scale_max", 10.0);

	obs_data_set_default_double(settings, "Kp", 0.95);
	obs_data_set_default_double(settings, "Ki", 0.3);
	obs_data_set_default_double(settings, "Td", 0.42);
	obs_data_set_default_double(settings, "Tdlpf", 2.0);
}

template <typename T> static inline bool samesign(const T &a, const T &b)
{
	if (a>0 && b>0)
		return true;
	if (a<0 && b<0)
		return true;
	return false;
}

static inline float sqf(float x) { return x*x; }

static void tick_filter(struct face_tracker_filter *s, float second)
{
	const float width = s->known_width;
	const float height = s->known_height;
	const float srwh = sqrtf(width * height);
	const float h = height / s->scale_max;
	const float w = width / s->scale_max;
	const float s2h = height / srwh;
	const float s2w = width / srwh;

	f3 e = s->detect_err;
	f3 e_int = e;
	for (int i=0; i<3; i++) {
		float x = e.v[i];
		float d = srwh * s->e_deadband.v[i];
		float n = srwh * s->e_nonlinear.v[i];
		if (std::abs(x) <= d)
			x = 0.0f;
		else if (std::abs(x) < (d + n)) {
			if (x > 0)
				x = +sqf(x - d) / (2.0f * n);
			else
				x = -sqf(x - d) / (2.0f * n);
		}
		else if (x > 0)
			x -= d + n * 0.5f;
		else
			x += d + n * 0.5f;
		if (second * s->ki > 1.0e-10) {
			if (s->filter_int.v[i] < 0.0f && e.v[i] > 0.0f)
				e_int.v[i] = std::min(e.v[i], -s->filter_int.v[i] / (second * s->ki));
			else if (s->filter_int.v[i] > 0.0f && e.v[i] < 0.0f)
				e_int.v[i] = std::max(e.v[i], -s->filter_int.v[i] / (second * s->ki));
			else
				e_int.v[i] = x;
		}
		e.v[i] = x;
	}

	s->filter_int_out += (e + s->filter_int) * (second * s->kp);
	s->filter_int += e_int * (second * s->ki);
	s->filter_lpf = (s->filter_lpf * s->tlpf + e * second) * (1.f/(s->tlpf + second));

	f3 u = s->filter_int_out + s->filter_lpf * s->klpf;

	for (int i=0; i<3; i++) {
		if (isnan(u.v[i]))
			u.v[i] = s->range_min.v[i];
		else if (u.v[i] < s->range_min.v[i]) {
			u.v[i] = s->range_min.v[i];
			if (s->filter_int_out.v[i] < s->range_min.v[i])
				s->filter_int_out.v[i] = s->range_min.v[i];
		}
		else if (u.v[i] > s->range_max.v[i]) {
			u.v[i] = s->range_max.v[i];
			if (s->filter_int_out.v[i] > s->range_max.v[i])
				s->filter_int_out.v[i] = s->range_max.v[i];
		}
	}

	s->crop_cur.x0 = u.v[0] - s2w * u.v[2] * 0.5f;
	s->crop_cur.x1 = u.v[0] + s2w * u.v[2] * 0.5f;
	s->crop_cur.y0 = u.v[1] - s2h * u.v[2] * 0.5f;
	s->crop_cur.y1 = u.v[1] + s2h * u.v[2] * 0.5f;
}

static void ftf_activate(void *data)
{
	auto *s = (struct face_tracker_filter*)data;
	s->is_active = true;
}

static void ftf_deactivate(void *data)
{
	auto *s = (struct face_tracker_filter*)data;
	s->is_active = false;
}

static inline void calculate_error(struct face_tracker_filter *s);

static void ftf_tick(void *data, float second)
{
	auto *s = (struct face_tracker_filter*)data;
	const bool was_rendered = s->rendered;
	if (s->detect_tick==s->tick_cnt)
		s->next_tick_stage_to_detector = s->tick_cnt + (int)(2.0f/second); // detect for each _ second(s).

	s->tick_cnt += 1;

	obs_source_t *target = obs_filter_get_target(s->context);
	if (!target)
		goto err;

	s->rendered = false;

	s->known_width = obs_source_get_base_width(target);
	s->known_height = obs_source_get_base_height(target);

	if (s->known_width<=0 || s->known_height<=0)
		goto err;

	if (s->crop_cur.x1<-1 || s->crop_cur.y1<-1) {
		ftf_reset_tracking(NULL, NULL, s);
		s->crop_cur.x0 = 0;
		s->crop_cur.y0 = 0;
		s->crop_cur.x1 = s->known_width;
		s->crop_cur.y1 = s->known_height;
	}
	else if (was_rendered) {
		s->range_min.v[0] = get_width(s->crop_cur) * 0.5f;
		s->range_max.v[0] = s->known_width - get_width(s->crop_cur) * 0.5f;
		s->range_min.v[1] = get_height(s->crop_cur) * 0.5f;
		s->range_max.v[1] = s->known_height - get_height(s->crop_cur) * 0.5f;
		s->range_min.v[2] = sqrtf(s->known_width*s->known_height) / s->scale_max;
		s->range_max.v[2] = sqrtf(s->known_width*s->known_height);
		calculate_error(s);
		tick_filter(s, second);
	}

	s->target_valid = true;
	return;
err:
	s->target_valid = false;
}

static inline void render_target(struct face_tracker_filter *s, obs_source_t *target, obs_source_t *parent)
{
	if (!s->texrender)
		s->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	const uint32_t cx = s->known_width, cy = s->known_height;
	gs_texrender_reset(s->texrender);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(s->texrender, cx, cy)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(s->texrender);
	}

	gs_blend_state_pop();

	s->staged = false;
}

static inline f3 ensure_range(f3 u, const struct face_tracker_filter *s)
{
	const float srwh = sqrtf(s->known_width * s->known_height);
	const float s2h = s->known_height / srwh;
	const float s2w = s->known_width / srwh;

	if (isnan(u.v[2]))
		u.v[2] = s->range_min.v[2];
	else if (u.v[2] < s->range_min.v[2])
		u.v[2] = s->range_min.v[2];
	else if (u.v[2] > s->range_max.v[2])
		u.v[2] = s->range_max.v[2];

	float x0 = u.v[0] - s2w * u.v[2] * 0.5f;
	float x1 = u.v[0] + s2w * u.v[2] * 0.5f;
	float y0 = u.v[1] - s2h * u.v[2] * 0.5f;
	float y1 = u.v[1] + s2h * u.v[2] * 0.5f;

	if (x0 < 0)
		u.v[0] += -x0;
	else if (x1 > s->known_width)
		u.v[0] -= x1 - s->known_width;

	if (y0 < 0)
		u.v[1] += -y0;
	else if (y1 > s->known_height)
		u.v[1] -= y1 - s->known_height;

	return u;
}

static inline void calculate_error(struct face_tracker_filter *s)
{
	const int width = s->known_width;
	const int height = s->known_height;
	f3 e_tot(0.0f, 0.0f, 0.0f);
	float sc_tot = 0.0f;
	bool found = false;
	std::deque<struct tracker_inst_s> &trackers = *s->trackers;
	for (int i=0; i<trackers.size(); i++) if (trackers[i].state == tracker_inst_s::tracker_state_available) {
		f3 r (trackers[i].rect);
		r.v[0] -= get_width(trackers[i].crop_rect) * s->track_x;
		r.v[1] += get_height(trackers[i].crop_rect) * s->track_y;
		r.v[2] /= s->track_z;
		r = ensure_range(r, s);
		f3 w (trackers[i].crop_rect);
		float score = trackers[i].rect.score * trackers[i].att;
		e_tot += (r-w) * score;
		sc_tot += score;
		found = true;
	}

	if (found)
		s->detect_err = e_tot * (1.0f / sc_tot);
	else
		s->detect_err = f3(0, 0, 0);
}

static inline void retire_tracker(struct face_tracker_filter *s, int ix)
{
	std::deque<struct tracker_inst_s> &trackers = *s->trackers;
	s->trackers_idlepool->push_back(trackers[ix]);
	trackers[ix].tracker->request_suspend();
	trackers.erase(trackers.begin()+ix);
}

static inline void attenuate_tracker(struct face_tracker_filter *s)
{
	std::deque<struct tracker_inst_s> &trackers = *s->trackers;
	std::vector<rect_s> &rects = *s->rects;

	for (int j=0; j<rects.size(); j++) {
		rect_s r = rects[j];
		float a0 = (r.x1 - r.x0) * (r.y1 - r.y0);
		float a_overlap_sum = 0;
		for (int i=trackers.size()-1; i>=0; i--) {
			if (trackers[i].state != tracker_inst_s::tracker_state_available)
				continue;
			float a = common_area(r, trackers[i].rect);
			a_overlap_sum += a;
			if (a>a0*0.1f && a_overlap_sum > a0*0.5f)
				retire_tracker(s, i);
		}
	}

	for (int i=0; i<trackers.size(); i++) {
		if (trackers[i].state != tracker_inst_s::tracker_state_available)
			continue;
		struct tracker_inst_s &t = trackers[i];

		float a1 = (t.rect.x1 - t.rect.x0) * (t.rect.y1 - t.rect.y0);
		float amax = a1*0.1f;
		for (int j=0; j<rects.size(); j++) {
			rect_s r = rects[j];
			float a0 = (r.x1 - r.x0) * (r.y1 - r.y0);
			float a = common_area(r, t.rect);
			if (a > amax) amax = a;
		}

		t.att *= powf(amax / a1, 0.1f); // if no faces, remove the tracker
	}

	float score_max = 1e-17f;
	for (int i=0; i<trackers.size(); i++) {
		if (trackers[i].state == tracker_inst_s::tracker_state_available) {
			float s = trackers[i].att * trackers[i].rect.score;
			if (s > score_max) score_max = s;
		}
	}

	for (int i=0; i<trackers.size(); ) {
		if (trackers[i].state == tracker_inst_s::tracker_state_available && trackers[i].att * trackers[i].rect.score < 1e-2f * score_max) {
			retire_tracker(s, i);
		}
		else
			i++;
	}
}

static inline void copy_detector_to_tracker(struct face_tracker_filter *s)
{
	std::deque<struct tracker_inst_s> &trackers = *s->trackers;
	int i_tracker;
	for (i_tracker=0; i_tracker < trackers.size(); i_tracker++)
		if (
				trackers[i_tracker].tick_cnt == s->detect_tick &&
				trackers[i_tracker].state==tracker_inst_s::tracker_state_e::tracker_state_reset_texture )
			break;
	if (i_tracker >= trackers.size())
		return;

	if (s->rects->size()<=0) {
		trackers.erase(trackers.begin() + i_tracker);
		return;
	}

	struct tracker_inst_s &t = trackers[i_tracker];

	struct rect_s r = (*s->rects)[0];
	int w = r.x1-r.x0;
	int h = r.y1-r.y0;
	r.x0 -= w * s->upsize_l;
	r.x1 += w * s->upsize_r;
	r.y0 -= h * s->upsize_t;
	r.y1 += h * s->upsize_b;
	t.tracker->set_position(r); // TODO: consider how to track two or more faces.
	t.tracker->start();
	t.state = tracker_inst_s::tracker_state_constructing;
}

static inline int stage_to_surface(struct face_tracker_filter *s)
{
	if (s->staged)
		return 0;

	uint32_t width = s->known_width;
	uint32_t height = s->known_height;
	if (width<=0 || height<=0)
		return 1;

	gs_texture_t *tex = gs_texrender_get_texture(s->texrender);
	if (!tex)
		return 2;

	if (!s->stagesurface ||
			width != gs_stagesurface_get_width(s->stagesurface) ||
			height != gs_stagesurface_get_height(s->stagesurface) ) {
		gs_stagesurface_destroy(s->stagesurface);
		s->stagesurface = gs_stagesurface_create(width, height, GS_RGBA);
	}

	gs_stage_texture(s->stagesurface, tex);

	s->staged = true;

	return 0;
}

static inline void stage_to_detector(struct face_tracker_filter *s)
{
	if (s->detect->trylock())
		return;

	// get previous results
	if (s->detector_in_progress) {
		s->detect->get_faces(*s->rects);
		attenuate_tracker(s);
		copy_detector_to_tracker(s);
		s->detector_in_progress = false;
	}

	if ((s->next_tick_stage_to_detector - s->tick_cnt) > 0) {
		// blog(LOG_INFO, "stage_to_detector: waiting next_tick_stage_to_detector=%d tick_cnt=%d", s->next_tick_stage_to_detector, s->tick_cnt);
		s->detect->unlock();
		return;
	}

	if (!stage_to_surface(s)) {
		uint8_t *video_data = NULL;
		uint32_t video_linesize;
		if (gs_stagesurface_map(s->stagesurface, &video_data, &video_linesize)) {
			uint32_t width = s->known_width;
			uint32_t height = s->known_height;
			s->detect->set_texture(video_data, video_linesize, width, height);
			gs_stagesurface_unmap(s->stagesurface);
			s->detect->signal();
			s->detector_in_progress = true;
			s->detect_tick = s->tick_cnt;

			struct tracker_inst_s t;
			if (s->trackers_idlepool->size() > 0) {
				t.tracker = (*s->trackers_idlepool)[0].tracker;
				(*s->trackers_idlepool)[0].tracker = NULL;
				s->trackers_idlepool->pop_front();
			}
			else
				t.tracker = new face_tracker_dlib();
			t.crop_tracker = s->crop_cur;
			t.state = tracker_inst_s::tracker_state_e::tracker_state_reset_texture;
			t.tick_cnt = s->tick_cnt;
			t.tracker->set_texture(video_data, video_linesize, width, height); // TODO: common texture object.
			s->trackers->push_back(t);
		}
	}

	s->detect->unlock();
}

static inline int stage_surface_to_tracker(struct face_tracker_filter *s, struct tracker_inst_s &t)
{
	if (int ret = stage_to_surface(s))
		return ret;

	uint8_t *video_data = NULL;
	uint32_t video_linesize;
	if (gs_stagesurface_map(s->stagesurface, &video_data, &video_linesize)) {
		uint32_t width = s->known_width;
		uint32_t height = s->known_height;
		t.tracker->set_texture(video_data, video_linesize, width, height);
		t.crop_tracker = s->crop_cur;
		gs_stagesurface_unmap(s->stagesurface);
		t.tracker->signal();
	}
	else
		return 1;
	return 0;
}

static inline void stage_to_trackers(struct face_tracker_filter *s)
{
	std::deque<struct tracker_inst_s> &trackers = *s->trackers;
	for (int i=0; i<trackers.size(); i++) {
		struct tracker_inst_s &t = trackers[i];
		if (t.state == tracker_inst_s::tracker_state_constructing) {
			if (!t.tracker->trylock()) {
				if (!stage_surface_to_tracker(s, t)) {
					t.crop_tracker = s->crop_cur;
					t.state = tracker_inst_s::tracker_state_first_track;
				}
				t.tracker->unlock();
				t.state = tracker_inst_s::tracker_state_first_track;
			}
		}
		else if (t.state == tracker_inst_s::tracker_state_first_track) {
			if (!t.tracker->trylock()) {
				t.tracker->get_face(t.rect);
				t.crop_rect = t.crop_tracker;
				// blog(LOG_INFO, "tracker_state_first_track: %p rect=%d %d %d %d %f", t.tracker, t.rect.x0, t.rect.y0, t.rect.x1, t.rect.y1, t.rect.score);
				t.att = 1.0f;
				stage_surface_to_tracker(s, t);
				t.tracker->signal();
				t.tracker->unlock();
				t.state = tracker_inst_s::tracker_state_available;
			}
		}
		else if (t.state == tracker_inst_s::tracker_state_available) {
			if (!t.tracker->trylock()) {
				t.tracker->get_face(t.rect);
				t.crop_rect = t.crop_tracker;
				// blog(LOG_INFO, "tracker_state_available: %p rect=%d %d %d %d %f", t.tracker, t.rect.x0, t.rect.y0, t.rect.x1, t.rect.y1, t.rect.score);
				stage_surface_to_tracker(s, t);
				t.tracker->signal();
				t.tracker->unlock();
			}
		}
	}
}

static inline void draw_frame(struct face_tracker_filter *s)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(s->texrender);
	if (!tex)
		return;

	uint32_t width = s->known_width;
	uint32_t height = s->known_height;

	// TODO: linear_srgb, 27 only?

	gs_matrix_push();
	if (width>0 && height>0) {
		const rectf_s &crop_cur = s->crop_cur;
		float scale = sqrtf((float)(width*height) / ((crop_cur.x1-crop_cur.x0) * (crop_cur.y1-crop_cur.y0)));
		struct matrix4 tr;
		matrix4_identity(&tr);
		matrix4_translate3f(&tr, &tr, -(crop_cur.x0+crop_cur.x1)*0.5f, -(crop_cur.y0+crop_cur.y1)*0.5f, 0.0f);
		matrix4_scale3f(&tr, &tr, scale, scale, 1.0f);
		matrix4_translate3f(&tr, &tr, width/2, height/2, 0.0f);
		if (!(s->debug_notrack && !s->is_active))
			gs_matrix_mul(&tr);

		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(image, tex);
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(tex, 0, width, height);
		}
	}

	if (s->debug_faces && !s->is_active) {
		effect = obs_get_base_effect(OBS_EFFECT_SOLID);
		while (gs_effect_loop(effect, "Solid")) {
			gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFF0000FF);
			for (int i=0; i<s->rects->size(); i++) {
				rect_s r = (*s->rects)[i];
				if (r.x0>=r.x1 || r.y0>=r.y1)
					continue;
				int w = r.x1-r.x0;
				int h = r.y1-r.y0;
				gs_render_start(true);
				gs_vertex2f(r.x0, r.y0);
				gs_vertex2f(r.x0, r.y1);
				gs_vertex2f(r.x1, r.y1);
				gs_vertex2f(r.x1, r.y0);
				gs_vertex2f(r.x0, r.y0);
				r.x0 -= w * s->upsize_l;
				r.x1 += w * s->upsize_r;
				r.y0 -= h * s->upsize_t;
				r.y1 += h * s->upsize_b;
				//gs_render_start(false);
				gs_vertex2f(r.x0, r.y0);
				gs_vertex2f(r.x0, r.y1);
				gs_vertex2f(r.x1, r.y1);
				gs_vertex2f(r.x1, r.y0);
				gs_vertex2f(r.x0, r.y0);
				gs_vertbuffer_t *vb = gs_render_save();
				gs_load_vertexbuffer(vb);
				gs_draw(GS_LINESTRIP, 0, 0);
				gs_vertexbuffer_destroy(vb);
			}
			gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFF00FF00);
			for (int i=0; i<s->trackers->size(); i++) {
				tracker_inst_s &t = (*s->trackers)[i];
				if (t.state != tracker_inst_s::tracker_state_available)
					continue;
				rect_s r = t.rect;
				if (r.x0>=r.x1 || r.y0>=r.y1)
					continue;
				int w = r.x1-r.x0;
				int h = r.y1-r.y0;
				gs_render_start(true);
				gs_vertex2f(r.x0, r.y0);
				gs_vertex2f(r.x0, r.y1);
				gs_vertex2f(r.x1, r.y1);
				gs_vertex2f(r.x1, r.y0);
				gs_vertex2f(r.x0, r.y0);
				gs_vertbuffer_t *vb = gs_render_save();
				gs_load_vertexbuffer(vb);
				gs_draw(GS_LINESTRIP, 0, 0);
				gs_vertexbuffer_destroy(vb);
			}
			if (s->debug_notrack) {
				gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFFFFFF00); // amber
				const rectf_s &r = s->crop_cur;
				gs_render_start(true);
				gs_vertex2f(r.x0, r.y0);
				gs_vertex2f(r.x0, r.y1);
				gs_vertex2f(r.x0, r.y1);
				gs_vertex2f(r.x1, r.y1);
				gs_vertex2f(r.x1, r.y1);
				gs_vertex2f(r.x1, r.y0);
				gs_vertex2f(r.x1, r.y0);
				gs_vertex2f(r.x0, r.y0);
				const float srwhr2 = sqrtf((r.x1-r.x0) * (r.y1-r.y0)) * 0.5f;
				const float rcx = (r.x0+r.x1)*0.5f + (r.x1-r.x0)*s->track_x;
				const float rcy = (r.y0+r.y1)*0.5f - (r.y1-r.y0)*s->track_y;
				gs_vertex2f(rcx-srwhr2*s->track_z, rcy);
				gs_vertex2f(rcx+srwhr2*s->track_z, rcy);
				gs_vertex2f(rcx, rcy-srwhr2*s->track_z);
				gs_vertex2f(rcx, rcy+srwhr2*s->track_z);
				gs_vertbuffer_t *vb = gs_render_save();
				gs_load_vertexbuffer(vb);
				gs_draw(GS_LINES, 0, 0);
				gs_vertexbuffer_destroy(vb);
			}
		}
	}

	gs_matrix_pop();
}

static void ftf_render(void *data, gs_effect_t *)
{
	auto *s = (struct face_tracker_filter*)data;
	if (!s->target_valid) {
		obs_source_skip_video_filter(s->context);
		return;
	}
	obs_source_t *target = obs_filter_get_target(s->context);
	obs_source_t *parent = obs_filter_get_parent(s->context);

	if (!target || !parent) {
		obs_source_skip_video_filter(s->context);
		return;
	}

	if (!s->rendered) {
		render_target(s, target, parent);
		stage_to_detector(s);
		stage_to_trackers(s);
		s->rendered = true;
	}

	draw_frame(s);
}

extern "C"
void register_face_tracker_filter()
{
	struct obs_source_info info = {};
	info.id = "face_tracker_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = ftf_get_name;
	info.create = ftf_create;
	info.destroy = ftf_destroy;
	info.update = ftf_update;
	info.get_properties = ftf_properties;
	info.get_defaults = ftf_get_defaults;
	info.activate = ftf_activate,
	info.deactivate = ftf_deactivate,
	info.video_tick = ftf_tick;
	info.video_render = ftf_render;
	obs_register_source(&info);
}
