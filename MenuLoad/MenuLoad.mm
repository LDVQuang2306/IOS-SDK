#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <ImageIO/ImageIO.h>

#include "Includes.h"

@interface MenuLoad()

@property (nonatomic, strong) ImGuiDrawView* ImGuiView;
@property (nonatomic, strong) UIImage* CachedGifImage;

- (ImGuiDrawView*)GetImGuiDrawView;

@end

static MenuLoad* GExtraInfo;

@interface MenuInteraction()

@end

@implementation MenuInteraction

- (BOOL)pointInside:(CGPoint)InPoint withEvent:(UIEvent*)InEvent
{
    ImGuiContext* CurrentContext = ImGui::GetCurrentContext();
    
    if (CurrentContext)
    {
        const ImVector<ImGuiWindow*>& AllWindows = CurrentContext->Windows;
        for (int32_t WindowIndex = 0; WindowIndex < AllWindows.Size; ++WindowIndex)
        {
            ImGuiWindow* CurrentWindow = AllWindows[WindowIndex];
            if (!CurrentWindow)
            {
                continue;
            }

            if (CurrentWindow->Active && !(CurrentWindow->Flags & ImGuiWindowFlags_NoInputs))
            {
                const CGRect WindowTouchArea = CGRectMake(
                    CurrentWindow->Pos.x,
                    CurrentWindow->Pos.y,
                    CurrentWindow->Size.x,
                    CurrentWindow->Size.y
                );
                
                if (CGRectContainsPoint(WindowTouchArea, InPoint))
                {
                    return [super pointInside:InPoint withEvent:InEvent];
                }
            }
        }
    }
    
    return NO;
}

- (void)touchesBegan:(NSSet<UITouch*>*)InTouches withEvent:(UIEvent*)InEvent
{
    [[GExtraInfo GetImGuiDrawView] updateIOWithTouchEvent:InEvent];
}

- (void)touchesMoved:(NSSet<UITouch*>*)InTouches withEvent:(UIEvent*)InEvent
{
    [[GExtraInfo GetImGuiDrawView] updateIOWithTouchEvent:InEvent];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)InTouches withEvent:(UIEvent*)InEvent
{
    [[GExtraInfo GetImGuiDrawView] updateIOWithTouchEvent:InEvent];
}

- (void)touchesEnded:(NSSet<UITouch*>*)InTouches withEvent:(UIEvent*)InEvent
{
    [[GExtraInfo GetImGuiDrawView] updateIOWithTouchEvent:InEvent];
}

@end

@implementation MenuLoad

bool bIsMenuOpened = false;

- (ImGuiDrawView*)GetImGuiDrawView
{
    return _ImGuiView;
}

+ (void)load
{
    [super load];

    // Load the menu 3 seconds after application launch, you can adjust this
    CallAfterSeconds(3)
    {
        GExtraInfo = [MenuLoad new];
        [GExtraInfo InitializeGestureRecognizers];
        
        // Initialize screen dimensions after touch registration
        ScreenRect.Init();
    });
}

- (void)InitializeGestureRecognizers
{
    UIView* const MainApplicationView = [UIApplication sharedApplication].windows[0].rootViewController.view;
    const CGRect ScreenBounds = [[UIScreen mainScreen] bounds];


    [self SetupHiddenRecordingComponents:ScreenBounds];
    [self InitializeImGuiDrawSystem];
    [self SetupTouchInteractionLayer:MainApplicationView];
    [self LoadAnimatedMenuButton:MainApplicationView];
    [self SetupInvisibleDraggableButton:MainApplicationView];
}

- (void)SetupHiddenRecordingComponents:(CGRect)InScreenBounds
{
    GHideRecordTextField = [[UITextField alloc] init];
    GHideRecordView = [[UIView alloc] initWithFrame:InScreenBounds];
    GHideRecordView.backgroundColor = [UIColor clearColor];
    GHideRecordView.userInteractionEnabled = YES;
    GHideRecordTextField.secureTextEntry = YES;
    [GHideRecordView addSubview:GHideRecordTextField];
    
    CALayer* const TextFieldLayer = GHideRecordTextField.layer;
    
    if ([TextFieldLayer.sublayers.firstObject.delegate isKindOfClass:[UIView class]])
    {
        GHideRecordView = (UIView*)TextFieldLayer.sublayers.firstObject.delegate;
    }
    else
    {
        GHideRecordView = nil;
    }

    if (GHideRecordView)
    {
        [[UIApplication sharedApplication].windows.firstObject addSubview:GHideRecordView];
    }
}

- (void)InitializeImGuiDrawSystem
{
    if (!_ImGuiView)
    {
        _ImGuiView = [[ImGuiDrawView alloc] init];
    }
    
    [ImGuiDrawView showChange:NO];
    [GHideRecordView addSubview:_ImGuiView.view];
}

- (void)SetupTouchInteractionLayer:(UIView*)InMainView
{
    GMenuTouchView = [[MenuInteraction alloc] initWithFrame:InMainView.frame];
    [InMainView addSubview:GMenuTouchView];
}

- (void)LoadAnimatedMenuButton:(UIView*)InMainView
{
    NSURL* const AnimatedImageURL = [NSURL URLWithString:@"https://lh3.googleusercontent.com/d/1NHl3ri6yuGHOxQLq0itVY77bd8jzmgG7=s440?authuser=0"];
    
    if (self.CachedGifImage)
    {
        [self CreateVisibleMenuButton:self.CachedGifImage InMainView:InMainView];
    }
    else
    {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool
            {
                NSData* const DownloadedImageData = [NSData dataWithContentsOfURL:AnimatedImageURL];
                if (DownloadedImageData)
                {
                    UIImage* ProcessedAnimatedImage = [self ProcessAnimatedImageData:DownloadedImageData];
                    if (ProcessedAnimatedImage)
                    {
                        self.CachedGifImage = ProcessedAnimatedImage;
                        
                        dispatch_async(dispatch_get_main_queue(), ^{
                            [self CreateVisibleMenuButton:ProcessedAnimatedImage InMainView:InMainView];
                        });
                    }
                }
            }
        });
    }
}

- (void)SetupInvisibleDraggableButton:(UIView*)InMainView
{
    const CGFloat ButtonSize = 75.0f;
    const CGFloat HalfButtonSize = ButtonSize * 0.5f;
    
    GInvisibleMenuButton = [UIButton buttonWithType:UIButtonTypeCustom];
    GInvisibleMenuButton.frame = CGRectMake(
        CGRectGetMidX(InMainView.frame) - HalfButtonSize,
        CGRectGetMidY(InMainView.frame) - HalfButtonSize,
        ButtonSize,
        ButtonSize
    );
    GInvisibleMenuButton.backgroundColor = [UIColor clearColor];
    
    [GInvisibleMenuButton addTarget:self
                             action:@selector(OnButtonDragged:withEvent:)
                   forControlEvents:UIControlEventTouchDragInside];

    UITapGestureRecognizer* MenuTapGesture = [[UITapGestureRecognizer alloc]
                                             initWithTarget:self
                                             action:@selector(OnMenuButtonTapped:)];
    [GInvisibleMenuButton addGestureRecognizer:MenuTapGesture];
    [InMainView addSubview:GInvisibleMenuButton];
}

- (void)CreateVisibleMenuButton:(UIImage*)InAnimatedImage InMainView:(UIView*)InMainView
{
    const CGFloat ButtonSize = 75.0f;
    const CGFloat HalfButtonSize = ButtonSize * 0.5f;
    const CGFloat CornerRadius = 25.0f;
    
    GVisibleMenuButton = [UIButton buttonWithType:UIButtonTypeCustom];
    GVisibleMenuButton.frame = CGRectMake(
        CGRectGetMidX(InMainView.frame) - HalfButtonSize,
        CGRectGetMidY(InMainView.frame) - HalfButtonSize,
        ButtonSize,
        ButtonSize
    );
    GVisibleMenuButton.backgroundColor = [UIColor clearColor];
    GVisibleMenuButton.layer.cornerRadius = CornerRadius;
    GVisibleMenuButton.clipsToBounds = YES;
    [GVisibleMenuButton setImage:InAnimatedImage forState:UIControlStateNormal];
    [GHideRecordView addSubview:GVisibleMenuButton];
}

- (UIImage* _Nullable)ProcessAnimatedImageData:(NSData* _Nonnull)InImageData
{
    if (!InImageData)
    {
        return nil;
    }
    
    CGImageSourceRef ImageSource = CGImageSourceCreateWithData((__bridge CFDataRef)InImageData, NULL);
    if (!ImageSource)
    {
        return nil;
    }
    
    const size_t FrameCount = CGImageSourceGetCount(ImageSource);
    NSMutableArray<UIImage*>* FrameImages = [NSMutableArray arrayWithCapacity:FrameCount];
    NSTimeInterval TotalAnimationDuration = 0.0f;
    
    for (size_t FrameIndex = 0; FrameIndex < FrameCount; FrameIndex++)
    {
        @autoreleasepool
        {
            const CGImageRef _Nullable CurrentFrameRef = CGImageSourceCreateImageAtIndex(ImageSource, FrameIndex, NULL);
            if (!CurrentFrameRef)
            {
                continue;
            }
            
            NSDictionary* FrameProperties = (__bridge_transfer NSDictionary*)CGImageSourceCopyPropertiesAtIndex(ImageSource, FrameIndex, NULL);
            NSDictionary* GifFrameProperties = FrameProperties[(NSString* _Nonnull)kCGImagePropertyGIFDictionary];
            
            NSNumber* FrameDelayTime = GifFrameProperties[(NSString* _Nonnull)kCGImagePropertyGIFUnclampedDelayTime];
            if (!FrameDelayTime)
            {
                FrameDelayTime = GifFrameProperties[(NSString* _Nonnull)kCGImagePropertyGIFDelayTime];
            }
            
            const float MinFrameDuration = 0.011f;
            const float DefaultFrameDuration = 0.100f;
            if (FrameDelayTime.floatValue < MinFrameDuration)
            {
                FrameDelayTime = @(DefaultFrameDuration);
            }
            
            TotalAnimationDuration += FrameDelayTime.floatValue;
            
            UIImage* ProcessedFrameImage = [UIImage imageWithCGImage:CurrentFrameRef
                                                               scale:[UIScreen mainScreen].scale
                                                         orientation:UIImageOrientationUp];
            [FrameImages addObject:ProcessedFrameImage];
            
            CGImageRelease(CurrentFrameRef);
        }
    }
    
    CFRelease(ImageSource);
    
    if (TotalAnimationDuration == 0.0f)
    {
        constexpr float DefaultFPS = 10.0f;
        TotalAnimationDuration = (1.0f / DefaultFPS) * FrameCount;
    }
    
    return [UIImage animatedImageWithImages:FrameImages duration:TotalAnimationDuration];
}

- (void)OnMenuButtonTapped:(UITapGestureRecognizer*)InTapGesture
{
    if (InTapGesture.state == UIGestureRecognizerStateEnded)
    {
        const BOOL bCurrentMenuVisibility = [ImGuiDrawView isMenuShowing];
        
        if (bCurrentMenuVisibility)
        {
//            settings.Save();
        }
        
        [ImGuiDrawView showChange:!bCurrentMenuVisibility];
    }
}

- (void)OnButtonDragged:(UIButton*)InDraggedButton withEvent:(UIEvent*)InDragEvent
{
    UITouch* DragTouch = [[InDragEvent touchesForView:InDraggedButton] anyObject];

    const CGPoint PreviousTouchLocation = [DragTouch previousLocationInView:InDraggedButton];
    const CGPoint CurrentTouchLocation = [DragTouch locationInView:InDraggedButton];
    const CGFloat HorizontalDelta = CurrentTouchLocation.x - PreviousTouchLocation.x;
    const CGFloat VerticalDelta = CurrentTouchLocation.y - PreviousTouchLocation.y;

    InDraggedButton.center = CGPointMake(
        InDraggedButton.center.x + HorizontalDelta,
        InDraggedButton.center.y + VerticalDelta
    );
    
    const CGRect ScreenBounds = [UIApplication sharedApplication].windows[0].rootViewController.view.bounds;
    [self ClampButtonToScreenBounds:InDraggedButton WithScreenBounds:ScreenBounds];
    
    GVisibleMenuButton.center = InDraggedButton.center;
    GVisibleMenuButton.frame = InDraggedButton.frame;
}

- (void)ClampButtonToScreenBounds:(UIButton*)InButton WithScreenBounds:(CGRect)InScreenBounds
{
    CGPoint ClampedCenter = InButton.center;
    
    if (ClampedCenter.x < 0)
    {
        ClampedCenter.x = 0;
    }
    else if (ClampedCenter.x > InScreenBounds.size.width)
    {
        ClampedCenter.x = InScreenBounds.size.width;
    }
    
    if (ClampedCenter.y < 0)
    {
        ClampedCenter.y = 0;
    }
    else if (ClampedCenter.y > InScreenBounds.size.height)
    {
        ClampedCenter.y = InScreenBounds.size.height;
    }
    
    InButton.center = ClampedCenter;
}

@end
