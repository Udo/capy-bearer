# BEARER starter

`site/examples/bearer-starter/` is a native Capy starter app.

The app uses `index.capy` as its front controller. It reads `request.route.path`, chooses a view, and renders the page shell directly in Capy.

Canonical routes:

- `/examples/bearer-starter/`
- `/examples/bearer-starter/?dashboard`
- `/examples/bearer-starter/?gauges`
- `/examples/bearer-starter/?features`
- `/examples/bearer-starter/?page1`
- `/examples/bearer-starter/?workspace`
- `/examples/bearer-starter/?workspace/projects`
- `/examples/bearer-starter/?page2-section1`

Direct requests to `/examples/bearer-starter/index.capy` also work. The router rejects unsafe route input and returns a Capy-rendered 404 page.

Static CSS, JavaScript, images, and fonts stay in the existing `themes/`, `views/`, `js/`, and `img/` directories. `lib/datastar.capy` remains as an optional native Capy helper for future server-sent event examples.
