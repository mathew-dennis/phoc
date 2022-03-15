#define G_LOG_DOMAIN "phoc-xwayland"

#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/config.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/xwayland.h>
#include "cursor.h"
#include "server.h"
#include "seat.h"
#include "view.h"
#include "xwayland.h"

static
bool is_moveable(PhocView *view)
{
	PhocServer *server = phoc_server_get_default ();
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view (view)->xwayland_surface;

	if (xwayland_surface->window_type == NULL)
		return true;

	for (guint i = 0; i < xwayland_surface->window_type_len; i++)
		if (xwayland_surface->window_type[i] != server->desktop->xwayland_atoms[NET_WM_WINDOW_TYPE_NORMAL] &&
		    xwayland_surface->window_type[i] != server->desktop->xwayland_atoms[NET_WM_WINDOW_TYPE_DIALOG])
			return false;

	return true;
}

static void set_active(PhocView *view, bool active) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;
	wlr_xwayland_surface_activate(xwayland_surface, active);
}

static void move(PhocView *view, double x, double y) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;

	if (!is_moveable (view))
		return;

	view_update_position(view, x, y);
	wlr_xwayland_surface_configure(xwayland_surface, x, y,
		xwayland_surface->width, xwayland_surface->height);
}

static void apply_size_constraints(PhocView *view,
		struct wlr_xwayland_surface *xwayland_surface, uint32_t width,
		uint32_t height, uint32_t *dest_width, uint32_t *dest_height) {
	*dest_width = width;
	*dest_height = height;

	if (view_is_maximized(view))
		return;

	struct wlr_xwayland_surface_size_hints *size_hints =
		xwayland_surface->size_hints;
	if (size_hints != NULL) {
		if (width < (uint32_t)size_hints->min_width) {
			*dest_width = size_hints->min_width;
		} else if (size_hints->max_width > 0 &&
				width > (uint32_t)size_hints->max_width) {
			*dest_width = size_hints->max_width;
		}
		if (height < (uint32_t)size_hints->min_height) {
			*dest_height = size_hints->min_height;
		} else if (size_hints->max_height > 0 &&
				height > (uint32_t)size_hints->max_height) {
			*dest_height = size_hints->max_height;
		}
	}
}

static void resize(PhocView *view, uint32_t width, uint32_t height) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;

	uint32_t constrained_width, constrained_height;
	apply_size_constraints(view, xwayland_surface, width, height, &constrained_width,
		&constrained_height);

	wlr_xwayland_surface_configure(xwayland_surface, xwayland_surface->x,
			xwayland_surface->y, constrained_width, constrained_height);
}

static void move_resize(PhocView *view, double x, double y,
		uint32_t width, uint32_t height) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;

	if (!is_moveable (view)) {
		x = view->box.x;
		y = view->box.y;
	}

	bool update_x = x != view->box.x;
	bool update_y = y != view->box.y;

	uint32_t constrained_width, constrained_height;
	apply_size_constraints(view, xwayland_surface, width, height, &constrained_width,
		&constrained_height);

	if (update_x) {
		x = x + width - constrained_width;
	}
	if (update_y) {
		y = y + height - constrained_height;
	}

	view->pending_move_resize.update_x = update_x;
	view->pending_move_resize.update_y = update_y;
	view->pending_move_resize.x = x;
	view->pending_move_resize.y = y;
	view->pending_move_resize.width = constrained_width;
	view->pending_move_resize.height = constrained_height;

	wlr_xwayland_surface_configure(xwayland_surface, x, y, constrained_width,
		constrained_height);
}

static void _close(PhocView *view) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;
	wlr_xwayland_surface_close(xwayland_surface);
}


static bool want_scaling(PhocView *view) {
	return false;
}

static bool want_auto_maximize(PhocView *view) {
	struct wlr_xwayland_surface *surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;

	if (surface->size_hints &&
	    (surface->size_hints->min_width > 0 && surface->size_hints->min_width == surface->size_hints->max_width) &&
	    (surface->size_hints->min_height > 0 && surface->size_hints->min_height == surface->size_hints->max_height))
		return false;

	return is_moveable(view);
}

static void set_maximized(PhocView *view, bool maximized) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;
	wlr_xwayland_surface_set_maximized(xwayland_surface, maximized);
}

static void set_fullscreen(PhocView *view, bool fullscreen) {
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_xwayland_surface_from_view(view)->xwayland_surface;
	wlr_xwayland_surface_set_fullscreen(xwayland_surface, fullscreen);
}

static void destroy(PhocView *view) {
	PhocXWaylandSurface *phoc_surface =
		phoc_xwayland_surface_from_view(view);
	wl_list_remove(&phoc_surface->destroy.link);
	wl_list_remove(&phoc_surface->request_configure.link);
	wl_list_remove(&phoc_surface->request_move.link);
	wl_list_remove(&phoc_surface->request_resize.link);
	wl_list_remove(&phoc_surface->request_maximize.link);
	wl_list_remove(&phoc_surface->set_title.link);
	wl_list_remove(&phoc_surface->set_class.link);
#ifdef PHOC_HAVE_WLR_SET_STARTUP_ID
	wl_list_remove(&phoc_surface->set_startup_id.link);
#endif
	wl_list_remove(&phoc_surface->map.link);
	wl_list_remove(&phoc_surface->unmap.link);
	free(phoc_surface);
}

static const PhocViewInterface view_impl = {
	.resize = resize,
	.move = move,
	.move_resize = move_resize,
	.want_scaling = want_scaling,
	.want_auto_maximize = want_auto_maximize,
	.set_active = set_active,
	.set_fullscreen = set_fullscreen,
	.set_maximized = set_maximized,
	.close = _close,
	.destroy = destroy,
};

static void handle_destroy(struct wl_listener *listener, void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, destroy);
	view_destroy(&phoc_surface->view);
}

static void handle_request_configure(struct wl_listener *listener, void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, request_configure);
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_surface->xwayland_surface;
	struct wlr_xwayland_surface_configure_event *event = data;

	view_update_position(&phoc_surface->view, event->x, event->y);

	wlr_xwayland_surface_configure(xwayland_surface, event->x, event->y,
		event->width, event->height);
}

static PhocSeat *guess_seat_for_view(PhocView *view) {
	// the best we can do is to pick the first seat that has the surface focused
	// for the pointer
	PhocServer *server = phoc_server_get_default ();
	PhocInput *input = server->input;

	for (GSList *elem = phoc_input_get_seats (input); elem; elem = elem->next) {
		PhocSeat *seat = PHOC_SEAT (elem->data);

		g_assert (PHOC_IS_SEAT (seat));
		if (seat->seat->pointer_state.focused_surface == view->wlr_surface) {
			return seat;
		}
	}
	return NULL;
}

static void handle_request_move(struct wl_listener *listener, void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, request_move);
	PhocView *view = &phoc_surface->view;
	PhocSeat *seat = guess_seat_for_view(view);

	if (!seat || phoc_seat_get_cursor(seat)->mode != PHOC_CURSOR_PASSTHROUGH) {
		return;
	}

	phoc_seat_begin_move(seat, view);
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, request_resize);
	PhocView *view = &phoc_surface->view;
	PhocSeat *seat = guess_seat_for_view(view);
	struct wlr_xwayland_resize_event *e = data;

	if (!seat || phoc_seat_get_cursor(seat)->mode != PHOC_CURSOR_PASSTHROUGH) {
		return;
	}
	phoc_seat_begin_resize(seat, view, e->edges);
}

static void handle_request_maximize(struct wl_listener *listener, void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, request_maximize);
	PhocView *view = &phoc_surface->view;
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_surface->xwayland_surface;

	bool maximized = xwayland_surface->maximized_vert &&
		xwayland_surface->maximized_horz;
	if (maximized) {
		view_maximize(view, NULL);
	} else {
		view_restore(view);
	}
}

static void handle_request_fullscreen(struct wl_listener *listener,
		void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, request_fullscreen);
	PhocView *view = &phoc_surface->view;
	struct wlr_xwayland_surface *xwayland_surface =
		phoc_surface->xwayland_surface;

	phoc_view_set_fullscreen(view, xwayland_surface->fullscreen, NULL);
}

static void handle_set_title(struct wl_listener *listener, void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, set_title);

	view_set_title(&phoc_surface->view,
		phoc_surface->xwayland_surface->title);
}

static void handle_set_class(struct wl_listener *listener, void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, set_class);

	view_set_app_id(&phoc_surface->view,
		phoc_surface->xwayland_surface->class);
}

#ifdef PHOC_HAVE_WLR_SET_STARTUP_ID
static void handle_set_startup_id(struct wl_listener *listener, void *data) {
	PhocServer *server = phoc_server_get_default ();

	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, set_startup_id);

	g_debug ("Got startup-id %s", phoc_surface->xwayland_surface->startup_id);
	phoc_phosh_private_notify_startup_id (server->desktop->phosh,
                                              phoc_surface->xwayland_surface->startup_id,
                                              PHOSH_PRIVATE_STARTUP_TRACKER_PROTOCOL_X11);
}
#endif /* PHOC_HAVE_WLR_SET_STARTUP_ID */

static void handle_surface_commit(struct wl_listener *listener, void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, surface_commit);
	PhocView *view = &phoc_surface->view;
	struct wlr_surface *wlr_surface = view->wlr_surface;

	phoc_view_apply_damage(view);

	int width = wlr_surface->current.width;
	int height = wlr_surface->current.height;
	view_update_size(view, width, height);

	double x = view->box.x;
	double y = view->box.y;
	if (view->pending_move_resize.update_x) {
		if (view_is_floating (view)) {
			x = view->pending_move_resize.x + view->pending_move_resize.width -
				width;
		} else {
			x = view->pending_move_resize.x;
		}
		view->pending_move_resize.update_x = false;
	}
	if (view->pending_move_resize.update_y) {
		if (view_is_floating (view)) {
			y = view->pending_move_resize.y + view->pending_move_resize.height -
				height;
		} else {
			y = view->pending_move_resize.y;
		}
		view->pending_move_resize.update_y = false;
	}
	view_update_position(view, x, y);
}

static void handle_map(struct wl_listener *listener, void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, map);
	struct wlr_xwayland_surface *surface = data;
	PhocView *view = &phoc_surface->view;

	view->box.x = surface->x;
	view->box.y = surface->y;
	view->box.width = surface->surface->current.width;
	view->box.height = surface->surface->current.height;

	phoc_surface->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&surface->surface->events.commit,
		&phoc_surface->surface_commit);

	if (surface->maximized_horz && surface->maximized_vert) {
		view_maximize(view, NULL);
	}
	view_auto_maximize(view);

	phoc_view_map(view, surface->surface);

	if (!surface->override_redirect) {
		if (surface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL) {
			view->decorated = true;
			view->border_width = 4;
			view->titlebar_height = 12;
		}

		view_setup(view);
	} else {
		view_initial_focus(view);
	}
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	PhocXWaylandSurface *phoc_surface =
		wl_container_of(listener, phoc_surface, unmap);
	PhocView *view = &phoc_surface->view;

	wl_list_remove(&phoc_surface->surface_commit.link);
	view_unmap(view);
}

void handle_xwayland_surface(struct wl_listener *listener, void *data) {
	PhocDesktop *desktop =
		wl_container_of(listener, desktop, xwayland_surface);

	struct wlr_xwayland_surface *surface = data;
	g_debug ("new xwayland surface: title=%s, class=%s, instance=%s",
		surface->title, surface->class, surface->instance);
	wlr_xwayland_surface_ping(surface);

	PhocXWaylandSurface *phoc_surface =
		calloc(1, sizeof(PhocXWaylandSurface));
	if (phoc_surface == NULL) {
		return;
	}

	view_init(&phoc_surface->view, &view_impl, ROOTS_XWAYLAND_VIEW, desktop);
	phoc_surface->view.box.x = surface->x;
	phoc_surface->view.box.y = surface->y;
	phoc_surface->view.box.width = surface->width;
	phoc_surface->view.box.height = surface->height;
	phoc_surface->xwayland_surface = surface;

	view_set_title(&phoc_surface->view, surface->title);
	view_set_app_id(&phoc_surface->view, surface->class);

	phoc_surface->destroy.notify = handle_destroy;
	wl_signal_add(&surface->events.destroy, &phoc_surface->destroy);
	phoc_surface->request_configure.notify = handle_request_configure;
	wl_signal_add(&surface->events.request_configure,
		&phoc_surface->request_configure);
	phoc_surface->map.notify = handle_map;
	wl_signal_add(&surface->events.map, &phoc_surface->map);
	phoc_surface->unmap.notify = handle_unmap;
	wl_signal_add(&surface->events.unmap, &phoc_surface->unmap);
	phoc_surface->request_move.notify = handle_request_move;
	wl_signal_add(&surface->events.request_move, &phoc_surface->request_move);
	phoc_surface->request_resize.notify = handle_request_resize;
	wl_signal_add(&surface->events.request_resize,
		&phoc_surface->request_resize);
	phoc_surface->request_maximize.notify = handle_request_maximize;
	wl_signal_add(&surface->events.request_maximize,
		&phoc_surface->request_maximize);
	phoc_surface->request_fullscreen.notify = handle_request_fullscreen;
	wl_signal_add(&surface->events.request_fullscreen,
		&phoc_surface->request_fullscreen);
	phoc_surface->set_title.notify = handle_set_title;
	wl_signal_add(&surface->events.set_title, &phoc_surface->set_title);
	phoc_surface->set_class.notify = handle_set_class;
	wl_signal_add(&surface->events.set_class,
			&phoc_surface->set_class);
#ifdef PHOC_HAVE_WLR_SET_STARTUP_ID
	phoc_surface->set_startup_id.notify = handle_set_startup_id;
	wl_signal_add(&surface->events.set_startup_id,
			&phoc_surface->set_startup_id);
#endif
}

PhocXWaylandSurface *phoc_xwayland_surface_from_view(
		PhocView *view) {
	assert(view->impl == &view_impl);
	return (PhocXWaylandSurface *)view;
}
