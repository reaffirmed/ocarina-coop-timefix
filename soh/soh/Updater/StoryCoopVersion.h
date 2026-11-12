#pragma once
// The Story Co-op release this source builds. The auto updater compares it with the newest release on GitHub.
// Bump it together with the release tag: the release workflow refuses a tag that does not match (v + this).
// (A test build can pass -DSTORY_COOP_VERSION="0.9.0" to pretend to be older and be offered the current release.)
#ifndef STORY_COOP_VERSION
#define STORY_COOP_VERSION "1.0.0"
#endif
