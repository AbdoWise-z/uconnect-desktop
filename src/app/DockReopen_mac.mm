#include "DockReopen.hpp"

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

namespace ucd {

void onDockReopen(std::function<void()> callback) {
    static std::function<void()> reopened;
    const bool hooked = static_cast<bool>(reopened);
    reopened = std::move(callback);
    // Qt's application delegate answers the reopen message; wrap its answer.
    id delegate = NSApp.delegate;
    if (hooked || !delegate) return;
    const SEL selector = @selector(applicationShouldHandleReopen:hasVisibleWindows:);
    const Class cls = [delegate class];
    using Handler = BOOL (*)(id, SEL, NSApplication*, BOOL);
    const Method method = class_getInstanceMethod(cls, selector);
    const Handler original = method ? reinterpret_cast<Handler>(method_getImplementation(method)) : nullptr;
    const IMP wrapper = imp_implementationWithBlock(^BOOL(id self, NSApplication* app, BOOL visibleWindows) {
        if (reopened) reopened();
        return original ? original(self, selector, app, visibleWindows) : YES;
    });
    if (method) method_setImplementation(method, wrapper);
    else class_addMethod(cls, selector, wrapper, "c@:@c");
}

}  // namespace ucd
