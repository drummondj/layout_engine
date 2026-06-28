#import <CoreVideo/CoreVideo.h>
#import <FlutterMacOS/FlutterMacOS.h>

@interface LayoutEngineTexture : NSObject <FlutterTexture>

@property(nonatomic, strong) NSTimer *loopTimer;

- (instancetype)initTextureWithRegistry:(id<FlutterTextureRegistry>)registry
                                 handle:(void *)handle
                                  width:(int)width
                                 height:(int)height;
- (int64_t)textureId;
- (void)tickAnimationFrame;
- (void)resizeWithWidth:(int)width height:(int)height;

@end
