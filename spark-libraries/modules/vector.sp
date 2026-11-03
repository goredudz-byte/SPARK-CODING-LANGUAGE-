$import "sys"


mut vec_fd = -1
mut vec_width = 0
mut vec_height = 0
mut vec_error = 0

fn vec_write(s: str) -> int
    if vec_fd < 0
        return -1
    let n = sys_write(vec_fd, s)
    if n < 0
        vec_error = sys_errno()
    n

fn vec_begin(path: str, width: int, height: int) -> int
    if vec_fd >= 0
        sys_close(vec_fd)
    vec_width = width
    vec_height = height
    vec_error = 0
    vec_fd = sys_open(path, 577, 420)
    if vec_fd < 0
        vec_error = sys_errno()
        return -1
    vec_write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
    vec_write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"")
    vec_write(str(width))
    vec_write("\" height=\"")
    vec_write(str(height))
    vec_write("\" viewBox=\"0 0 ")
    vec_write(str(width))
    vec_write(" ")
    vec_write(str(height))
    vec_write("\">\n")
    0

fn vec_background(color: str) -> int
    vec_write("<rect x=\"0\" y=\"0\" width=\"")
    vec_write(str(vec_width))
    vec_write("\" height=\"")
    vec_write(str(vec_height))
    vec_write("\" fill=\"")
    vec_write(color)
    vec_write("\"/>\n")

fn vec_line(x1: int, y1: int, x2: int, y2: int, color: str, width: int) -> int
    vec_write("<line x1=\"")
    vec_write(str(x1))
    vec_write("\" y1=\"")
    vec_write(str(y1))
    vec_write("\" x2=\"")
    vec_write(str(x2))
    vec_write("\" y2=\"")
    vec_write(str(y2))
    vec_write("\" stroke=\"")
    vec_write(color)
    vec_write("\" stroke-width=\"")
    vec_write(str(width))
    vec_write("\"/>\n")

fn vec_rect(x: int, y: int, width: int, height: int, fill: str) -> int
    vec_write("<rect x=\"")
    vec_write(str(x))
    vec_write("\" y=\"")
    vec_write(str(y))
    vec_write("\" width=\"")
    vec_write(str(width))
    vec_write("\" height=\"")
    vec_write(str(height))
    vec_write("\" fill=\"")
    vec_write(fill)
    vec_write("\"/>\n")

fn vec_rect_outline(x: int, y: int, width: int, height: int, color: str, stroke_width: int) -> int
    vec_write("<rect x=\"")
    vec_write(str(x))
    vec_write("\" y=\"")
    vec_write(str(y))
    vec_write("\" width=\"")
    vec_write(str(width))
    vec_write("\" height=\"")
    vec_write(str(height))
    vec_write("\" fill=\"none\" stroke=\"")
    vec_write(color)
    vec_write("\" stroke-width=\"")
    vec_write(str(stroke_width))
    vec_write("\"/>\n")

fn vec_circle(cx: int, cy: int, radius: int, fill: str) -> int
    vec_write("<circle cx=\"")
    vec_write(str(cx))
    vec_write("\" cy=\"")
    vec_write(str(cy))
    vec_write("\" r=\"")
    vec_write(str(radius))
    vec_write("\" fill=\"")
    vec_write(fill)
    vec_write("\"/>\n")

fn vec_circle_outline(cx: int, cy: int, radius: int, color: str, stroke_width: int) -> int
    vec_write("<circle cx=\"")
    vec_write(str(cx))
    vec_write("\" cy=\"")
    vec_write(str(cy))
    vec_write("\" r=\"")
    vec_write(str(radius))
    vec_write("\" fill=\"none\" stroke=\"")
    vec_write(color)
    vec_write("\" stroke-width=\"")
    vec_write(str(stroke_width))
    vec_write("\"/>\n")

fn vec_ellipse(cx: int, cy: int, rx: int, ry: int, fill: str) -> int
    vec_write("<ellipse cx=\"")
    vec_write(str(cx))
    vec_write("\" cy=\"")
    vec_write(str(cy))
    vec_write("\" rx=\"")
    vec_write(str(rx))
    vec_write("\" ry=\"")
    vec_write(str(ry))
    vec_write("\" fill=\"")
    vec_write(fill)
    vec_write("\"/>\n")

fn vec_text(x: int, y: int, size: int, color: str, value: str) -> int
    vec_write("<text x=\"")
    vec_write(str(x))
    vec_write("\" y=\"")
    vec_write(str(y))
    vec_write("\" font-family=\"sans-serif\" font-size=\"")
    vec_write(str(size))
    vec_write("\" fill=\"")
    vec_write(color)
    vec_write("\">")
    vec_write(value)
    vec_write("</text>\n")

fn vec_path(d: str, stroke: str, fill: str, stroke_width: int) -> int
    vec_write("<path d=\"")
    vec_write(d)
    vec_write("\" stroke=\"")
    vec_write(stroke)
    vec_write("\" fill=\"")
    vec_write(fill)
    vec_write("\" stroke-width=\"")
    vec_write(str(stroke_width))
    vec_write("\"/>\n")

fn vec_end() -> int
    if vec_fd < 0
        return -1
    vec_write("</svg>\n")
    let result = sys_close(vec_fd)
    vec_fd = -1
    result

fn vec_last_error() -> int
    vec_error
