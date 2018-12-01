youtube-dl 'https://www.youtube.com/watch?v=Wa2fBiCEzTc'
FILE='MULTI ANGLE _ George Evans pops up with a 92nd-minute winner in Blackburn!-Wa2fBiCEzTc.mp4'
ffmpeg -y -ss 0:03.290 -t 0:37 -i "$FILE" -c:v mjpeg -an angle1.mp4    
ffmpeg -y -ss 0:40 -t 0:40 -i "$FILE" -c:v mjpeg -an angle2.mp4   
ffmpeg -y -ss 1:12.880 -i "$FILE" -c:v mjpeg -an angle3.mp4
ffmpeg -y -ss 0:07 -i ./angle3.mp4 -c:v copy -copyts -start_at_zero angle3-cut.mp4                    
ffmpeg -y -copyts -i angle1.mp4 -i angle2.mp4 -i angle3-cut.mp4 -map 0:0 -map 1:0 -map 2:0 -c:v copy multiangle.mp4 

