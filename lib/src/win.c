#include <string.h>
#include <cairo/cairo-xcb.h>
#include "win.h"

// connects to the display server and opens ewmh interface
short win_init_xcb(struct properties* properties)
{
	short result;
	properties->ewmh_conn = malloc(sizeof(xcb_ewmh_connection_t));
	properties->xcb_conn = xcb_connect(NULL,
			&(properties->screen_id));
	properties->xcb_cookie = xcb_ewmh_init_atoms(properties->xcb_conn,
			properties->ewmh_conn);
	properties->screen = xcb_setup_roots_iterator(xcb_get_setup(
				properties->xcb_conn)).data;
	result = xcb_ewmh_init_atoms_replies(properties->ewmh_conn,
			properties->xcb_cookie,
			NULL);
	return (result == 1) ? 0 : -1;
}

// uses X11 event system to communicate with threads
void win_ping(struct properties* properties)
{
	xcb_property_notify_event_t* event;

	if(properties->window == 0)
	{
		return;
	}

	event = calloc(32, 1);
	event->response_type = XCB_PROPERTY_NOTIFY;
	xcb_send_event(properties->xcb_conn,
		0,
		properties->window,
		XCB_EVENT_MASK_EXPOSURE,
		(char*)event);
	xcb_flush(properties->xcb_conn);
	free(event);
}

// uses X11 event system to communicate with threads
void win_render(struct properties* properties)
{
	xcb_expose_event_t* event;

	if(properties->window == 0)
	{
		return;
	}

	event = calloc(32, 1);
	event->response_type = XCB_EXPOSE;
	xcb_send_event(properties->xcb_conn,
		0,
		properties->window,
		XCB_EVENT_MASK_EXPOSURE,
		(char*)event);
	xcb_flush(properties->xcb_conn);
	free(event);
}

// adds an event to the events array corresponding to thread_id
void win_add_event(struct properties* properties,
	uint8_t event_id,
	short thread_id)
{
	short array_size;

	// checks if the thread lacks an event array, then allocates it
	if(properties->plugins_events[thread_id] == NULL)
	{
		properties->plugins_events_size[thread_id] = 1;
		properties->plugins_events[thread_id] = malloc(sizeof(uint8_t));
		properties->plugins_events[thread_id][0] = event_id;
	}
	// otherwise just uses the existing one and appends the given event
	else
	{
		array_size = properties->plugins_events_size[thread_id];
		properties->plugins_events[thread_id] = realloc(
				properties->plugins_events[thread_id],
				(array_size + 1) * (sizeof(uint8_t)));
		properties->plugins_events[thread_id][array_size] = event_id;
		++(properties->plugins_events_size[thread_id]);
	}
}

// updates the root window's event masks so the main thread can
// receive an event on substructure updates and X property changes
static short win_update_root_events(struct properties* properties)
{
	xcb_generic_error_t* error;
	xcb_screen_t* screen;
	xcb_void_cookie_t cookie;
	uint32_t value;
	value = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
		| XCB_EVENT_MASK_PROPERTY_CHANGE;
	screen = xcb_setup_roots_iterator(xcb_get_setup(
				properties->xcb_conn)).data;
	cookie = xcb_change_window_attributes_checked(properties->xcb_conn,
			screen->root,
			XCB_CW_EVENT_MASK,
			&value);
	error = xcb_request_check(properties->xcb_conn, cookie);

	if(error != NULL)
	{
		return -1;
	}

	return 0;
}

// gets i3's "root" window containing all the user's windows
// used when option "root_win" is set to "i3" in excalibar.cfg
static xcb_window_t win_get_i3_root(struct properties* properties)
{
	xcb_generic_error_t* error;
	uint16_t i;
	xcb_connection_t* xcb_conn;
	xcb_screen_t* screen;
	xcb_window_t window;
	// children list
	xcb_query_tree_cookie_t cookie_tree;
	xcb_query_tree_reply_t* reply_tree;
	xcb_window_t* tree;
	uint16_t tree_size;
	// name
	xcb_get_property_cookie_t cookie_name;
	xcb_get_property_reply_t* reply_name;
	int name_len;
	xcb_conn = properties->xcb_conn;
	window = XCB_NONE;
	// gets the root window children tree
	screen = xcb_setup_roots_iterator(xcb_get_setup(xcb_conn)).data;
	cookie_tree = xcb_query_tree(xcb_conn, screen->root);
	reply_tree = xcb_query_tree_reply(xcb_conn, cookie_tree, &error);

	if(error != NULL)
	{
		return XCB_NONE;
	}

	tree = xcb_query_tree_children(reply_tree);
	tree_size = reply_tree->children_len;

	// processes the children list to find the window named "i3"
	for(i = 0; i < tree_size; ++i)
	{
		// gets the child's name
		cookie_name = xcb_ewmh_get_wm_name(properties->ewmh_conn, tree[i]);
		reply_name = xcb_get_property_reply(xcb_conn, cookie_name, &error);

		if(error != NULL)
		{
			return XCB_NONE;
		}

		name_len = xcb_get_property_value_length(reply_name);

		if((name_len != 0)
			&& (strncmp("i3",
					(char*)xcb_get_property_value(reply_name),
					name_len) == 0))
		{
			window = tree[i];
			free(reply_name);
			break;
		}

		free(reply_name);
	}

	free(reply_tree);
	return window;
}

static short win_restack(struct properties* properties)
{
	uint32_t mask;
	uint32_t values[2];
	mask = XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;

	if(strcmp(properties->config.bar_root_win, "i3") == 0)
	{
		values[0] = win_get_i3_root(properties);

		if(values[0] == XCB_NONE)
		{
			values[0] = properties->screen->root;
		}
	}
	else
	{
		values[0] = properties->screen->root;
	}

	values[1] = XCB_STACK_MODE_ABOVE;

	if(values[0] != XCB_NONE)
	{
		xcb_configure_window_checked(properties->xcb_conn,
			properties->window,
			mask,
			values);
	}
	else
	{
		return -1;
	}

	return 0;
}

static void win_set_strut(struct properties* properties)
{
	xcb_ewmh_wm_strut_partial_t strut = {0};
	xcb_ewmh_connection_t* ewmh_conn;
	ewmh_conn = properties->ewmh_conn;
	strut.top = properties->win_height;
	strut.top_start_x = 0;
	strut.top_end_x = properties->config.bar_width;
	xcb_ewmh_set_wm_strut(ewmh_conn,
		properties->window,
		0,
		0,
		properties->win_height,
		0);
	xcb_ewmh_set_wm_strut_partial(ewmh_conn,
		properties->window,
		strut);
}

static short win_set_attr(struct properties* properties)
{
	xcb_generic_error_t* error;
	short i;
	xcb_connection_t* xcb_conn = properties->xcb_conn;
	xcb_ewmh_connection_t* ewmh_conn = properties->ewmh_conn;
	xcb_window_t window = properties->window;
	xcb_intern_atom_cookie_t atom_cookie;
	xcb_intern_atom_reply_t* atom_reply;
	xcb_atom_t atoms[3];
	char* atoms_names[3] = {"_NET_WM_WINDOW_TYPE_DOCK",
			"_NET_WM_STATE_STICKY",
			"_NET_WM_STATE_ABOVE"
		};

	for(i = 0; i < 3; ++i)
	{
		atom_cookie = xcb_intern_atom(xcb_conn,
				0,
				strlen(atoms_names[i]),
				atoms_names[i]);
		atom_reply = xcb_intern_atom_reply(xcb_conn,
				atom_cookie,
				&error);

		if(error != NULL)
		{
			return -1;
		}

		atoms[i] = atom_reply->atom;
		free(atom_reply);
	}

	xcb_ewmh_set_wm_window_type(ewmh_conn, window, 1, &atoms[0]);
	xcb_ewmh_set_wm_state(ewmh_conn, window, 2, &atoms[1]);
	xcb_ewmh_set_wm_desktop(ewmh_conn, window, -1);
	return 0;
}

static xcb_visualtype_t* win_get_visual_type(struct properties*
	properties)
{
	xcb_visualtype_t* visual_type;
	xcb_screen_t* screen;
	xcb_depth_iterator_t depth_iter;
	visual_type = NULL;
	screen = properties->screen;
	depth_iter = xcb_screen_allowed_depths_iterator(screen);
	xcb_visualtype_iterator_t visual_iter;

	while(depth_iter.rem && visual_type == NULL)
	{
		visual_iter = xcb_depth_visuals_iterator(depth_iter.data);

		while(visual_iter.rem && visual_type == NULL)
		{
			if(screen->root_visual == visual_iter.data->visual_id)
			{
				visual_type = visual_iter.data;
				break;
			}

			xcb_visualtype_next(&visual_iter);
		}

		xcb_depth_next(&depth_iter);
	}

	return visual_type;
}

//ChatGPT
xcb_visualtype_t* find_argb32_visual(xcb_connection_t* connection, xcb_screen_t* screen) {
    xcb_depth_iterator_t depth_iter;
    xcb_visualtype_iterator_t visual_iter;

    // Iterate over all the depths for the screen
    depth_iter = xcb_screen_allowed_depths_iterator(screen);
    while (depth_iter.rem) {
        if (depth_iter.data->depth == 32) {  // Look for 32-bit depth
            // Now check the visuals for this depth
            visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
            while (visual_iter.rem) {
	      if(visual_iter.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR &&
		 visual_iter.data->bits_per_rgb_value == 8){
                // Return the first ARGB32 visual we find
                return visual_iter.data;
	      }
            }
        }
        xcb_depth_next(&depth_iter);
    }
    return NULL;  // No ARGB32 visual found
}

//see i3/i3, x.c and xcb.c

xcb_window_t create_window(xcb_connection_t *conn, struct properties* properties,
                           uint16_t depth, xcb_visualid_t visual, uint16_t window_class,
                           uint32_t mask, uint32_t *values, xcb_window_t root) {
    xcb_window_t result = xcb_generate_id(conn);

    /* If the window class is XCB_WINDOW_CLASS_INPUT_ONLY, we copy depth and
     * visual id from the parent window. */
    if (window_class == XCB_WINDOW_CLASS_INPUT_ONLY) {
        depth = XCB_COPY_FROM_PARENT;
        visual = XCB_COPY_FROM_PARENT;
    }

    xcb_void_cookie_t gc_cookie = xcb_create_window(conn,
                                                    depth,
                                                    result,                                  /* the window id */
                                                    root,                                    /* parent == root */
                                                    0, 0,
						    properties->config.bar_width,
						    properties->config.bar_height, /* dimensions */
                                                    0,                                       /* border = 0, we draw our own */
                                                    window_class,
                                                    visual,
                                                    mask,
                                                    values);

    xcb_generic_error_t *error = xcb_request_check(conn, gc_cookie);
    if (error != NULL) {
      //ELOG("Could not create window. Error code: %d.\n", error->error_code);
      exit(1);
    }

    return result;
}

xcb_drawable_t x_con_init(xcb_connection_t* xcb_conn, struct properties* properties, xcb_screen_t* screen, xcb_visualid_t visual)
{
  uint32_t mask = 0;
  uint32_t values[5];

  xcb_colormap_t win_colormap = xcb_generate_id(xcb_conn);
  xcb_create_colormap(xcb_conn, XCB_COLORMAP_ALLOC_NONE, win_colormap, screen->root, visual);
  //con->colormap=cmap

  mask |= XCB_CW_BACK_PIXEL;
  values[0] = screen->black_pixel;

  mask |= XCB_CW_BORDER_PIXEL;
  values[1] = screen->black_pixel;

  /* our own frames should not be managed */
  mask |= XCB_CW_OVERRIDE_REDIRECT;
  values[2] = 1;
    

  #define FRAME_EVENT_MASK (XCB_EVENT_MASK_BUTTON_PRESS | /* …mouse is pressed/released */                       \
                          XCB_EVENT_MASK_BUTTON_RELEASE |                                                        \
                          XCB_EVENT_MASK_POINTER_MOTION |        /* …mouse is moved */                         \
                          XCB_EVENT_MASK_EXPOSURE |              /* …our window needs to be redrawn */         \
                          XCB_EVENT_MASK_STRUCTURE_NOTIFY |      /* …the frame gets destroyed */               \
                          XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | /* …the application tries to resize itself */ \
                          XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |   /* …subwindows get notifies */                \
                          XCB_EVENT_MASK_ENTER_WINDOW)           /* …user moves cursor inside our window */

  /* see include/xcb.h for the FRAME_EVENT_MASK */
  mask |= XCB_CW_EVENT_MASK;
  values[3] = FRAME_EVENT_MASK & ~XCB_EVENT_MASK_ENTER_WINDOW;

  mask |= XCB_CW_COLORMAP;
  values[4] = win_colormap;

  xcb_drawable_t frame_id = create_window(xcb_conn, properties, 32, visual,
					XCB_WINDOW_CLASS_INPUT_OUTPUT,
					  mask, values, screen->root);
  return frame_id;
}

short win_create(struct properties* properties)
{
  xcb_connection_t* xcb_conn = properties->xcb_conn;
  xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(xcb_conn)).data;
  xcb_visualtype_t* visual = find_argb32_visual(xcb_conn, screen);
  xcb_drawable_t window = x_con_init(xcb_conn, properties, screen, visual->visual_id);
  properties->window = window;
  properties->screen = screen;
  // configures the window
  win_update_root_events(properties);
  win_set_attr(properties);
  win_set_strut(properties);
  win_restack(properties);
  char *m_win_name = "excalibar";
  xcb_change_property(xcb_conn,
		      XCB_PROP_MODE_REPLACE,
		      window,
		      XCB_ATOM_WM_CLASS,
		      XCB_ATOM_STRING,
		      8,
		      strlen(m_win_name),
		      m_win_name);
  cairo_surface_t* surface = cairo_xcb_surface_create(xcb_conn,
				     window,
				     visual,
				     properties->config.bar_width,
				     properties->config.bar_height);
  cairo_t* cairo_conn = cairo_create(surface);
  cairo_surface_flush(surface);
  xcb_map_window(xcb_conn, window);
  xcb_flush(xcb_conn);
  // saves the structures
  properties->cairo_conn = cairo_conn;
  properties->cairo_surface = surface;
  return 0;
}
/*
short win_make(struct properties* properties)
{
	xcb_connection_t* xcb_conn;
	xcb_drawable_t window;
	xcb_screen_t* screen;
	xcb_visualtype_t* visual_type;
	cairo_t* cairo_conn;
	cairo_surface_t* surface;
	uint32_t mask;
	xcb_conn = properties->xcb_conn;
	// creates the window
	screen = xcb_setup_roots_iterator(xcb_get_setup(xcb_conn)).data;
	// establish visuals supporting transparency
	visual_type = find_argb32_visual(xcb_conn, screen);
	if (visual_type == NULL){
	  exit(1);
	} else {
	  printf("Visual found: depth=%d, class=%d, bits_per_rgb_value=%d\n",
		 32, visual_type->_class, visual_type->bits_per_rgb_value);
	}

	xcb_colormap_t colormap = xcb_generate_id(xcb_conn);
	xcb_create_colormap(xcb_conn, XCB_COLORMAP_ALLOC_NONE, colormap, screen->root, visual_type);//visual_type->visual_id);
	
	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
	uint32_t value_list[] = { 0, // bg color transp. pixel
	      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_KEY_PRESS,
	      colormap };


	uint32_t old_mask = XCB_CW_EVENT_MASK;
	uint32_t old_value = XCB_EVENT_MASK_EXPOSURE
		| XCB_EVENT_MASK_VISIBILITY_CHANGE
		| XCB_EVENT_MASK_BUTTON_PRESS
		| XCB_EVENT_MASK_STRUCTURE_NOTIFY
		| XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
		| XCB_EVENT_MASK_FOCUS_CHANGE;

	// X-plow-zean
	window = xcb_generate_id(xcb_conn);
	xcb_create_window(xcb_conn,
		XCB_COPY_FROM_PARENT,
		window,
		screen->root,
		0,
		0,
		properties->config.bar_width,
		properties->config.bar_height,
		0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT,
		visual_type->visual_id,
		mask,
			 value_list);
			  /*screen->root_visual,
		old_mask,
		&old_value);//
	// saves the structures
	properties->window = window;
	properties->screen = screen;
	// configures the window
	win_update_root_events(properties);
	win_set_attr(properties);
	win_set_strut(properties);
	win_restack(properties);
	char *m_win_name = "excalibar";
	xcb_change_property(xcb_conn,
			    XCB_PROP_MODE_REPLACE,
			    window,
			    XCB_ATOM_WM_CLASS,
			    XCB_ATOM_STRING,
			    8,
			    strlen(m_win_name),
			    m_win_name);
	// maps and starts cairo
	
	//visual_type = win_get_visual_type(properties);
	

	
	/*
	xcb_create_window(connection,
                  32,  // Depth (must match visual depth)
                  window, screen->root,
                  0, 0, width, height,
                  0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                  );//
	surface = cairo_xcb_surface_create(xcb_conn,
			window,
			visual_type,
			properties->config.bar_width,
			properties->config.bar_height);
	cairo_conn = cairo_create(surface);
	cairo_surface_flush(surface);
	xcb_map_window(xcb_conn, window);
	xcb_flush(xcb_conn);
	// saves the structures
	properties->cairo_conn = cairo_conn;
	properties->cairo_surface = surface;
	return 0;
}
*/
