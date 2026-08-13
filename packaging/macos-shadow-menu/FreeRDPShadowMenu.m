#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ServiceManagement/ServiceManagement.h>

typedef NS_ENUM(NSInteger, ShadowServerState)
{
	ShadowServerStopped,
	ShadowServerStarting,
	ShadowServerRunning,
	ShadowServerFailed
};

@interface ShadowAppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property(nonatomic, strong) NSStatusItem* statusItem;
@property(nonatomic, strong) NSMenu* menu;
@property(nonatomic, strong) NSTask* serverTask;
@property(nonatomic, strong) NSFileHandle* serverLog;
@property(nonatomic, assign) ShadowServerState state;
@property(nonatomic, copy) NSString* failureReason;
@property(nonatomic, assign) BOOL stopping;
@property(nonatomic, assign) BOOL wantsServerRunning;
@property(nonatomic, assign) NSUInteger restartGeneration;
@property(nonatomic, assign) NSUInteger restartFailures;
@property(nonatomic, strong) NSDate* serverStartedAt;
@end

@implementation ShadowAppDelegate

- (NSDictionary*)config
{
	NSURL* url = [[NSBundle mainBundle] URLForResource:@"ShadowConfig" withExtension:@"plist"];
	NSDictionary* config = url ? [NSDictionary dictionaryWithContentsOfURL:url] : nil;
	return config ? config : @{};
}

- (NSURL*)logURL
{
	NSString* configured = [self config][@"LogFile"];
	NSString* fallback = [@"~/Library/Logs/FreeRDPShadow/server.log" stringByExpandingTildeInPath];
	return [NSURL fileURLWithPath:configured ? configured : fallback];
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
	(void)notification;
	if ([[[NSProcessInfo processInfo] arguments] containsObject:@"--unregister-login-item"])
	{
		[self unregisterLoginItem];
		[NSApp terminate:nil];
		return;
	}
	if ([[[NSProcessInfo processInfo] arguments] containsObject:@"--register-login-item"])
	{
		[self registerLoginItemIfNeeded];
		fprintf(stdout, "Login-item status: %ld\n", (long)[SMAppService mainAppService].status);
		[NSApp terminate:nil];
		return;
	}

	self.state = ShadowServerStopped;
	self.menu = [[NSMenu alloc] init];
	self.menu.delegate = self;
	self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
	self.statusItem.menu = self.menu;
	[self registerLoginItemIfNeeded];
	[self requestPermissions:nil];
	[self updateMenuBar];
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
	               dispatch_get_main_queue(), ^{
	                 [self startServer:nil];
	               });
}

- (void)applicationWillTerminate:(NSNotification*)notification
{
	(void)notification;
	self.wantsServerRunning = NO;
	self.restartGeneration++;
	[self stopServerAndWait];
}

- (void)menuWillOpen:(NSMenu*)menu
{
	(void)menu;
	[self rebuildMenu];
}

- (void)registerLoginItemIfNeeded
{
	SMAppService* service = [SMAppService mainAppService];
	if ((service.status != SMAppServiceStatusEnabled) &&
	    (service.status != SMAppServiceStatusRequiresApproval))
	{
		NSError* error = nil;
		if (![service registerAndReturnError:&error])
		{
			self.state = ShadowServerFailed;
			self.failureReason = [NSString stringWithFormat:@"login item: %@", error.localizedDescription];
		}
	}
}

- (void)unregisterLoginItem
{
	NSError* error = nil;
	if (![[SMAppService mainAppService] unregisterAndReturnError:&error])
		fprintf(stderr, "Failed to unregister login item: %s\n", error.localizedDescription.UTF8String);
}

- (BOOL)loginItemEnabled
{
	return [SMAppService mainAppService].status == SMAppServiceStatusEnabled;
}

- (void)rebuildMenu
{
	[self.menu removeAllItems];
	NSMenuItem* status = [[NSMenuItem alloc] initWithTitle:[self statusDescription]
	                                              action:nil
	                                       keyEquivalent:@""];
	status.enabled = NO;
	[self.menu addItem:status];

	NSMenuItem* listener = [[NSMenuItem alloc] initWithTitle:@"Listener: 127.0.0.1:3390"
	                                                action:nil
	                                         keyEquivalent:@""];
	listener.enabled = NO;
	[self.menu addItem:listener];
	[self.menu addItem:[NSMenuItem separatorItem]];

	BOOL shouldStop = self.serverTask.running || self.wantsServerRunning;
	NSString* toggleTitle = shouldStop ? @"Stop RDP Server" : @"Start RDP Server";
	SEL toggleAction = shouldStop ? @selector(stopServer:) : @selector(startServer:);
	[self.menu addItemWithTitle:toggleTitle action:toggleAction keyEquivalent:@""];
	[self.menu addItemWithTitle:@"Open Server Log" action:@selector(openLog:) keyEquivalent:@""];
	[self.menu addItem:[NSMenuItem separatorItem]];

	NSMenuItem* login = [[NSMenuItem alloc] initWithTitle:@"Launch at Login"
	                                             action:@selector(toggleLoginItem:)
	                                      keyEquivalent:@""];
	login.state = [self loginItemEnabled] ? NSControlStateValueOn : NSControlStateValueOff;
	[self.menu addItem:login];
	[self.menu addItemWithTitle:@"Open Login Items Settings"
	                       action:@selector(openLoginItems:)
	                keyEquivalent:@""];
	[self.menu addItemWithTitle:@"Request Capture and Input Permissions"
	                       action:@selector(requestPermissions:)
	                keyEquivalent:@""];
	[self.menu addItemWithTitle:@"Open Screen Recording Settings"
	                       action:@selector(openScreenRecording:)
	                keyEquivalent:@""];
	[self.menu addItemWithTitle:@"Open Accessibility Settings"
	                       action:@selector(openAccessibility:)
	                keyEquivalent:@""];
	[self.menu addItem:[NSMenuItem separatorItem]];
	[self.menu addItemWithTitle:@"Quit FreeRDP Shadow" action:@selector(quit:) keyEquivalent:@"q"];
}

- (NSString*)statusDescription
{
	switch (self.state)
	{
		case ShadowServerStopped:
			return @"RDP server: Off";
		case ShadowServerStarting:
			return self.failureReason
			           ? [NSString stringWithFormat:@"RDP server: Restarting — %@", self.failureReason]
			           : @"RDP server: Starting…";
		case ShadowServerRunning:
			return @"RDP server: On";
		case ShadowServerFailed:
			return [NSString stringWithFormat:@"RDP server: Error — %@",
			                                  self.failureReason ? self.failureReason : @"see log"];
	}
}

- (void)updateMenuBar
{
	BOOL running = self.serverTask.running;
	self.statusItem.button.title = running ? @"RDP ●" : (self.wantsServerRunning ? @"RDP ↻" : @"RDP ○");
	self.statusItem.button.toolTip = [self statusDescription];
}

- (void)appendSupervisorLog:(NSString*)message
{
	NSFileHandle* log = [NSFileHandle fileHandleForWritingAtPath:[self logURL].path];
	if (!log)
		return;
	[log seekToEndOfFile];
	NSString* line = [NSString stringWithFormat:@"[FreeRDP Shadow menu] %@\n", message];
	[log writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
	[log closeFile];
}

- (void)scheduleServerRestart:(NSString*)reason
{
	if (!self.wantsServerRunning)
		return;

	self.restartFailures++;
	NSUInteger shift = self.restartFailures > 6 ? 5 : self.restartFailures - 1;
	NSTimeInterval delay = (NSTimeInterval)(1UL << shift);
	if (delay > 30.0)
		delay = 30.0;
	NSUInteger generation = ++self.restartGeneration;
	self.state = ShadowServerStarting;
	self.failureReason =
	    [NSString stringWithFormat:@"%@; retrying in %.0f second%@", reason, delay,
	                               delay == 1.0 ? @"" : @"s"];
	[self appendSupervisorLog:self.failureReason];
	[self updateMenuBar];

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
	               dispatch_get_main_queue(), ^{
	                 if ((generation != self.restartGeneration) || !self.wantsServerRunning ||
	                     self.serverTask.running)
		                 return;
	                 [self launchServer];
	               });
}

- (void)launchServer
{
	if (self.serverTask.running || !self.wantsServerRunning)
		return;
	if (!CGPreflightScreenCaptureAccess())
	{
		self.wantsServerRunning = NO;
		self.state = ShadowServerFailed;
		self.failureReason = @"Screen Recording permission required";
		[self updateMenuBar];
		return;
	}
	if (!AXIsProcessTrusted())
	{
		self.wantsServerRunning = NO;
		self.state = ShadowServerFailed;
		self.failureReason = @"Accessibility permission required";
		[self updateMenuBar];
		return;
	}

	NSDictionary* config = [self config];
	NSString* executable = config[@"ServerExecutable"];
	NSString* connectCommand = config[@"ConnectDisplayCommand"];
	NSString* disconnectCommand = config[@"DisconnectDisplayCommand"];
	if (!executable || !connectCommand || !disconnectCommand)
	{
		self.wantsServerRunning = NO;
		self.state = ShadowServerFailed;
		self.failureReason = @"ShadowConfig.plist is incomplete";
		[self updateMenuBar];
		return;
	}

	NSFileManager* manager = [NSFileManager defaultManager];
	if (![manager isExecutableFileAtPath:executable] ||
	    ![manager isExecutableFileAtPath:connectCommand] ||
	    ![manager isExecutableFileAtPath:disconnectCommand])
	{
		self.wantsServerRunning = NO;
		self.state = ShadowServerFailed;
		self.failureReason = @"server or display helper is not executable";
		[self updateMenuBar];
		return;
	}

	NSError* error = nil;
	NSURL* logURL = [self logURL];
	if (![manager createDirectoryAtURL:[logURL URLByDeletingLastPathComponent]
	       withIntermediateDirectories:YES
	                        attributes:nil
	                             error:&error])
	{
		self.wantsServerRunning = NO;
		self.state = ShadowServerFailed;
		self.failureReason = error.localizedDescription;
		[self updateMenuBar];
		return;
	}

	if (![manager fileExistsAtPath:logURL.path])
		[manager createFileAtPath:logURL.path contents:nil attributes:nil];
	NSFileHandle* log = [NSFileHandle fileHandleForWritingAtPath:logURL.path];
	[log seekToEndOfFile];

	NSTask* task = [[NSTask alloc] init];
	task.executableURL = [NSURL fileURLWithPath:executable];
	task.currentDirectoryURL = [NSURL fileURLWithPath:[executable stringByDeletingLastPathComponent]];
	task.arguments = @[
		@"/bind-address:127.0.0.1", @"/port:3390", @"/sec:rdp", @"-auth", @"-gfx",
		@"-rfx", @"-nsc", @"/log-level:INFO"
	];
	NSMutableDictionary* environment = [[[NSProcessInfo processInfo] environment] mutableCopy];
	environment[@"FREERDP_MAC_SHADOW_CONNECT_DISPLAY_COMMAND"] = connectCommand;
	environment[@"FREERDP_MAC_SHADOW_DISCONNECT_DISPLAY_COMMAND"] = disconnectCommand;
	task.environment = environment;
	task.standardOutput = log;
	task.standardError = log;

	__weak ShadowAppDelegate* weakSelf = self;
	task.terminationHandler = ^(NSTask* finished) {
	  dispatch_async(dispatch_get_main_queue(), ^{
	    ShadowAppDelegate* strongSelf = weakSelf;
	    if (!strongSelf)
		    return;
	    [log closeFile];
	    if (strongSelf.serverLog == log)
		    strongSelf.serverLog = nil;
	    if (strongSelf.serverTask == finished)
		    strongSelf.serverTask = nil;
	    if (strongSelf.stopping || !strongSelf.wantsServerRunning)
	    {
		    strongSelf.state = ShadowServerStopped;
		    strongSelf.failureReason = nil;
		    strongSelf.restartFailures = 0;
	    }
	    else
	    {
		    NSTimeInterval uptime = [strongSelf.serverStartedAt timeIntervalSinceNow] * -1.0;
		    if (uptime >= 60.0)
			    strongSelf.restartFailures = 0;
		    NSString* reason = finished.terminationReason == NSTaskTerminationReasonUncaughtSignal
		                           ? [NSString stringWithFormat:@"signal %d", finished.terminationStatus]
		                           : [NSString stringWithFormat:@"exit %d", finished.terminationStatus];
		    [strongSelf scheduleServerRestart:reason];
	    }
	    strongSelf.stopping = NO;
	    strongSelf.serverStartedAt = nil;
	    [strongSelf updateMenuBar];
	  });
	};

	self.stopping = NO;
	self.state = ShadowServerStarting;
	self.serverLog = log;
	self.serverTask = task;
	if ([task launchAndReturnError:&error])
	{
		self.serverStartedAt = [NSDate date];
		self.state = ShadowServerRunning;
		self.failureReason = nil;
	}
	else
	{
		[log closeFile];
		self.serverLog = nil;
		self.serverTask = nil;
		[self scheduleServerRestart:error.localizedDescription];
	}
	[self updateMenuBar];
}

- (void)startServer:(id)sender
{
	(void)sender;
	if (self.serverTask.running)
		return;
	self.wantsServerRunning = YES;
	self.stopping = NO;
	self.restartFailures = 0;
	self.restartGeneration++;
	self.failureReason = nil;
	[self launchServer];
}

- (void)stopServer:(id)sender
{
	(void)sender;
	self.wantsServerRunning = NO;
	self.restartGeneration++;
	self.restartFailures = 0;
	if (!self.serverTask.running)
	{
		self.state = ShadowServerStopped;
		self.failureReason = nil;
		[self updateMenuBar];
		return;
	}
	self.stopping = YES;
	[self.serverTask interrupt];
}

- (void)stopServerAndWait
{
	self.wantsServerRunning = NO;
	self.restartGeneration++;
	NSTask* task = self.serverTask;
	if (!task.running)
		return;
	self.stopping = YES;
	[task interrupt];
	NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
	while (task.running && [deadline timeIntervalSinceNow] > 0)
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
	if (task.running)
		[task terminate];
}

- (void)openLog:(id)sender
{
	(void)sender;
	[[NSWorkspace sharedWorkspace] openURL:[self logURL]];
}

- (void)toggleLoginItem:(id)sender
{
	(void)sender;
	SMAppService* service = [SMAppService mainAppService];
	NSError* error = nil;
	BOOL success = service.status == SMAppServiceStatusEnabled
	                   ? [service unregisterAndReturnError:&error]
	                   : [service registerAndReturnError:&error];
	if (!success)
	{
		self.state = ShadowServerFailed;
		self.failureReason = [NSString stringWithFormat:@"login item: %@", error.localizedDescription];
		[self updateMenuBar];
	}
}

- (void)openLoginItems:(id)sender
{
	(void)sender;
	[SMAppService openSystemSettingsLoginItems];
}

- (void)requestPermissions:(id)sender
{
	(void)sender;
	if (!CGPreflightScreenCaptureAccess())
		(void)CGRequestScreenCaptureAccess();
	NSDictionary* options = @{ (__bridge NSString*)kAXTrustedCheckOptionPrompt : @YES };
	(void)AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
}

- (void)openPrivacyAnchor:(NSString*)anchor
{
	NSString* value = [NSString
	    stringWithFormat:@"x-apple.systempreferences:com.apple.preference.security?%@", anchor];
	NSURL* url = [NSURL URLWithString:value];
	if (url)
		[[NSWorkspace sharedWorkspace] openURL:url];
}

- (void)openScreenRecording:(id)sender
{
	(void)sender;
	[self openPrivacyAnchor:@"Privacy_ScreenCapture"];
}

- (void)openAccessibility:(id)sender
{
	(void)sender;
	[self openPrivacyAnchor:@"Privacy_Accessibility"];
}

- (void)quit:(id)sender
{
	(void)sender;
	[NSApp terminate:nil];
}

@end

int main(int argc, const char* argv[])
{
	(void)argc;
	(void)argv;
	@autoreleasepool
	{
		NSApplication* application = [NSApplication sharedApplication];
		ShadowAppDelegate* delegate = [[ShadowAppDelegate alloc] init];
		application.delegate = delegate;
		[application run];
	}
	return 0;
}
