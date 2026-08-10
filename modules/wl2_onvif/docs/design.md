# Design

Each Winglib2 runtime lazily owns one `wlonvif::Runtime`. Native cold tasks start
only after Winglib2 operation accounting begins. Their completions are posted to
the Winglib2 JavaScript thread, where promises settle and accounting ends.

QuickJS session, Media, PTZ, and Event client objects share a native session handle. Explicit
`close()` is authoritative; native destruction is a non-blocking backstop. The
Winglib2 shutdown hook closes the native runtime and drains accepted completion
work before QuickJS teardown. Dynamic unloading is intentionally marked unsafe.

PullPoint subscriptions and managed streams retain their adapter session and are
registered as weak children of it. Explicit session close and runtime shutdown
schedule child cleanup first. Stream close is posted to the native executor
because upstream joins its polling worker during close; neither explicit close
nor a QuickJS finalizer joins that worker on the JavaScript thread. Promise
settlement is posted back through Winglib2 operation accounting.
