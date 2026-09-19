#include "graphics/presentation/window/loadingStatus.h"

#include "SDL_syswm.h"

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <string>

namespace Libs::Graphics {

namespace {

AVAudioPlayer* loading_audio = nil;
NSTimeInterval loading_started = 0.0;

}

void LoadingStatusUpdate(SDL_Window* window, std::string_view title, std::string_view title_id,
	                     std::string_view artwork_path) {
	SDL_SysWMinfo info {};
	SDL_VERSION(&info.version);
	if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE || info.subsystem != SDL_SYSWM_UIKIT ||
	    info.info.uikit.window == nullptr) {
		return;
	}

	constexpr NSInteger root_tag     = 0x4d505330;
	constexpr NSInteger title_tag    = 0x4d505331;
	constexpr NSInteger progress_tag = 0x4d505332;
	constexpr NSInteger metrics_tag  = 0x4d505333;
	auto* ui_window = info.info.uikit.window;
	auto* root      = static_cast<UIView*>([ui_window viewWithTag:root_tag]);
	if (root == nil) {
		loading_started                         = NSProcessInfo.processInfo.systemUptime;
		root                                      = [[UIView alloc] init];
		root.tag                                  = root_tag;
		root.translatesAutoresizingMaskIntoConstraints = NO;
		root.userInteractionEnabled               = NO;
		root.backgroundColor = [UIColor colorWithRed:0.025 green:0.040 blue:0.025 alpha:1.0];

		auto* artwork = [[UIImageView alloc] init];
		artwork.translatesAutoresizingMaskIntoConstraints = NO;
		artwork.contentMode                       = UIViewContentModeScaleToFill;
		artwork.clipsToBounds                     = YES;
		auto* icon_path = [NSString stringWithUTF8String:std::string(artwork_path).c_str()];
		auto* game_root = [[icon_path stringByDeletingLastPathComponent]
		    stringByDeletingLastPathComponent];
		artwork.image = [UIImage imageWithContentsOfFile:icon_path];
		[root addSubview:artwork];

		auto* title_label = [[UILabel alloc] init];
		title_label.tag                            = title_tag;
		title_label.translatesAutoresizingMaskIntoConstraints = NO;
		title_label.textColor                      = UIColor.whiteColor;
		title_label.font                           = [UIFont systemFontOfSize:26.0 weight:UIFontWeightSemibold];
		title_label.textAlignment                  = NSTextAlignmentCenter;
		title_label.shadowColor                    = [UIColor colorWithWhite:0.0 alpha:0.9];
		title_label.shadowOffset                   = CGSizeMake(0.0, 2.0);
		[root addSubview:title_label];

		auto* progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
		progress.tag                            = progress_tag;
		progress.translatesAutoresizingMaskIntoConstraints = NO;
		progress.trackTintColor                 = [UIColor colorWithWhite:0.0 alpha:0.42];
		progress.progressTintColor              = [UIColor colorWithWhite:1.0 alpha:0.88];
		progress.accessibilityLabel             = NSLocalizedString(@"Estimated loading progress", nil);
		[root addSubview:progress];

		auto* metrics = [[UILabel alloc] init];
		metrics.tag                            = metrics_tag;
		metrics.translatesAutoresizingMaskIntoConstraints = NO;
		metrics.textColor                      = [UIColor colorWithWhite:0.90 alpha:1.0];
		metrics.font                           = [UIFont systemFontOfSize:15.0 weight:UIFontWeightRegular];
		metrics.textAlignment                  = NSTextAlignmentCenter;
		metrics.shadowColor                    = [UIColor colorWithWhite:0.0 alpha:0.9];
		metrics.shadowOffset                   = CGSizeMake(0.0, 2.0);
		[root addSubview:metrics];

		[ui_window addSubview:root];
		[NSLayoutConstraint activateConstraints:@[
			[root.leadingAnchor constraintEqualToAnchor:ui_window.leadingAnchor],
			[root.trailingAnchor constraintEqualToAnchor:ui_window.trailingAnchor],
			[root.topAnchor constraintEqualToAnchor:ui_window.topAnchor],
			[root.bottomAnchor constraintEqualToAnchor:ui_window.bottomAnchor],
			[artwork.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
			[artwork.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
			[artwork.topAnchor constraintEqualToAnchor:root.topAnchor],
			[artwork.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
			[title_label.leadingAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.leadingAnchor constant:32.0],
			[title_label.trailingAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.trailingAnchor constant:-32.0],
			[title_label.bottomAnchor constraintEqualToAnchor:progress.topAnchor constant:-12.0],
			[progress.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
			[progress.widthAnchor constraintEqualToAnchor:root.widthAnchor multiplier:0.56],
			[progress.bottomAnchor constraintEqualToAnchor:metrics.topAnchor constant:-12.0],
			[metrics.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
			[metrics.widthAnchor constraintEqualToAnchor:progress.widthAnchor],
			[metrics.bottomAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.bottomAnchor constant:-28.0]
		]];

		auto* music_path = [game_root stringByAppendingPathComponent:
		    @"data/resource_packs/vanilla_music/sounds/music/menu/menu1.ogg"];
		NSError* audio_error = nil;
		loading_audio = [[AVAudioPlayer alloc] initWithContentsOfURL:
		    [NSURL fileURLWithPath:music_path] error:&audio_error];
		loading_audio.numberOfLoops = -1;
		loading_audio.volume = 0.28;
		const bool prepared = [loading_audio prepareToPlay];
		const bool playing = [loading_audio play];
		std::printf("Haumea:LoadingAudio:Info: prepared=%d playing=%d error=%s\n", prepared,
		            playing, audio_error != nil ? audio_error.localizedDescription.UTF8String : "none");
	}

	auto* title_label = static_cast<UILabel*>([root viewWithTag:title_tag]);
	auto* progress    = static_cast<UIProgressView*>([root viewWithTag:progress_tag]);
	auto* metrics     = static_cast<UILabel*>([root viewWithTag:metrics_tag]);
	auto* game_title  = [NSString stringWithUTF8String:std::string(title).c_str()];
	auto* identifier  = [NSString stringWithUTF8String:std::string(title_id).c_str()];
	auto* samples_key = [@"loading.durationSamples." stringByAppendingString:identifier];
	NSArray<NSNumber*>* samples = [NSUserDefaults.standardUserDefaults arrayForKey:samples_key];
	const auto elapsed_seconds = NSProcessInfo.processInfo.systemUptime - loading_started;
	title_label.text  = [NSString localizedStringWithFormat:NSLocalizedString(@"Starting %@", nil),
	                                                        game_title];
	if (samples.count > 0) {
		double total_duration = 0.0;
		for (NSNumber* sample in samples) {
			total_duration += sample.doubleValue;
		}
		const auto average_duration = total_duration / static_cast<double>(samples.count);
		metrics.text = [NSString localizedStringWithFormat:
		    NSLocalizedString(@"Elapsed %.1f s. Estimated total %.1f s", nil), elapsed_seconds,
		    average_duration];
		const auto fraction = static_cast<float>(elapsed_seconds / average_duration);
		[progress setProgress:std::min(fraction, 1.0F) animated:YES];
		progress.hidden = NO;
		progress.accessibilityValue = [NSString localizedStringWithFormat:
		    NSLocalizedString(@"%.0f percent", nil), std::min(fraction * 100.0F, 100.0F)];
	} else {
		metrics.text = [NSString localizedStringWithFormat:
		    NSLocalizedString(@"Elapsed %.1f s", nil), elapsed_seconds];
		progress.hidden = YES;
	}
	[ui_window bringSubviewToFront:root];
}

void LoadingStatusFinish(SDL_Window* window, std::string_view title_id) {
	SDL_SysWMinfo info {};
	SDL_VERSION(&info.version);
	if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE || info.subsystem != SDL_SYSWM_UIKIT ||
	    info.info.uikit.window == nullptr) {
		return;
	}

	constexpr NSInteger root_tag = 0x4d505330;
	auto* ui_window = info.info.uikit.window;
	auto* root = [ui_window viewWithTag:root_tag];
	if (root == nil) {
		return;
	}
	auto* identifier = [NSString stringWithUTF8String:std::string(title_id).c_str()];
	auto* samples_key = [@"loading.durationSamples." stringByAppendingString:identifier];
	NSArray<NSNumber*>* stored = [NSUserDefaults.standardUserDefaults arrayForKey:samples_key];
	NSMutableArray<NSNumber*>* samples =
	    stored != nil ? [stored mutableCopy] : [[NSMutableArray alloc] init];
	const auto elapsed_seconds = NSProcessInfo.processInfo.systemUptime - loading_started;
	[samples addObject:@(elapsed_seconds)];
	if (samples.count > 5) {
		[samples removeObjectAtIndex:0];
	}
	[NSUserDefaults.standardUserDefaults setObject:samples forKey:samples_key];
	[root removeFromSuperview];
	[loading_audio stop];
	loading_audio = nil;
	loading_started = 0.0;
}

} // namespace Libs::Graphics
