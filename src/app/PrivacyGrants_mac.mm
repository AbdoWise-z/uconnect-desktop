// macOS: forget privacy grants on record for another build of this app. The
// reasoning, and the rule for which, are in core/PrivacyGrants.hpp.

#include "core/PrivacyGrants.hpp"

#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace ucd::privacy {

namespace {

// Kept in the app's defaults domain, which like the grants is per bundle
// identifier (not per --profile).
NSString* const kRequirementKey = @"UCDPrivacyGrantsRequirement";

// This build's designated requirement as text: what macOS matches a grant
// against. `cdhash H"..."` for an ad-hoc build, the identifier and certificate
// for a signed one. Empty if the running code is not signed at all.
QString designatedRequirement() {
    QString text;
    SecCodeRef self = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &self) != errSecSuccess || !self) return text;
    SecRequirementRef requirement = nullptr;
    // A SecCodeRef stands in for the static code of the same program.
    if (SecCodeCopyDesignatedRequirement((SecStaticCodeRef)self, kSecCSDefaultFlags, &requirement) == errSecSuccess
        && requirement) {
        CFStringRef string = nullptr;
        if (SecRequirementCopyString(requirement, kSecCSDefaultFlags, &string) == errSecSuccess && string) {
            text = QString::fromCFString(string);
            CFRelease(string);
        }
        CFRelease(requirement);
    }
    CFRelease(self);
    return text;
}

bool forget(const QString& service, NSString* bundleId) {
    @try {
        NSTask* task = [[NSTask alloc] init];
        task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/tccutil"];
        task.arguments = @[@"reset", service.toNSString(), bundleId];
        task.standardOutput = NSFileHandle.fileHandleWithNullDevice;
        task.standardError = NSFileHandle.fileHandleWithNullDevice;
        NSError* error = nil;
        if (![task launchAndReturnError:&error]) return false;
        [task waitUntilExit];
        return task.terminationStatus == 0;
    } @catch (NSException*) {
        return false;
    }
}

}  // namespace

QStringList forgetStaleGrants() {
    @autoreleasepool {
        // Only an app bundle has grants filed under an identifier.
        NSString* bundleId = NSBundle.mainBundle.bundleIdentifier;
        if (bundleId.length == 0) return {};
        const QString current = designatedRequirement();
        if (current.isEmpty()) return {};
        NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
        const QString recorded = QString::fromNSString([defaults stringForKey:kRequirementKey] ?: @"");

        // Checking never asks the user anything.
        const QList<Grant> grants{
            {QStringLiteral("ScreenCapture"), true, static_cast<bool>(CGPreflightScreenCaptureAccess())},
            {QStringLiteral("Accessibility"), true, static_cast<bool>(AXIsProcessTrusted())},
            {QStringLiteral("ListenEvent"), true, static_cast<bool>(CGPreflightListenEventAccess())},
            {QStringLiteral("PostEvent"), true, static_cast<bool>(CGPreflightPostEventAccess())},
            {QStringLiteral("Camera"), false, false},
            {QStringLiteral("Microphone"), false, false},
        };
        QStringList forgotten;
        for (const QString& service : staleGrants(recorded, current, grants)) {
            if (forget(service, bundleId)) forgotten << service;
        }
        if (recorded != current) [defaults setObject:current.toNSString() forKey:kRequirementKey];
        return forgotten;
    }
}

}  // namespace ucd::privacy
