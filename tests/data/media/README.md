# Test media

Three tiny samples, committed so that the tests which need a real picture or video also run on a
clean checkout (`tests/test_media.h` gives their path to the tests). They were made with ffmpeg and
carry no metadata:

```
ffmpeg -f lavfi -i "testsrc=size=96x72:rate=1"    -frames:v 1 -map_metadata -1 -fflags +bitexact -flags:v +bitexact photo1.png
ffmpeg -f lavfi -i "smptebars=size=96x72:rate=1"  -frames:v 1 -map_metadata -1 -fflags +bitexact -flags:v +bitexact photo2.png
ffmpeg -f lavfi -i "testsrc2=size=160x120:rate=25:duration=2" -an -c:v libx264 -preset veryslow -crf 36 \
       -pix_fmt yuv420p -map_metadata -1 -fflags +bitexact -flags:v +bitexact -movflags +faststart clip.mp4
```

The two pictures must differ (some tests count two distinct files). The clip only has to be a
valid H.264 video the server's FFmpeg can decode. Do not put large media here: the repository
ignores `/test_media/` for exactly that.
