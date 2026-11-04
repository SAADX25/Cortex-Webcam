# Avatar camera — Cortex Studio 0.9.7

## العربية
1. افتح Cortex Sender على الهاتف واترك الرأس والكتفين والمرفقين والرسغين داخل الكادر.
2. فعّل Enable avatar، ثم اختر Live2D Cubism وحمّل Haru أو موديل model3.json.
3. اختر Camera tracking. الحركة التجريبية وحركات idle لا تقود الشخصية في هذا الوضع.
4. انظر مستقيمًا واضغط Set neutral face مرة واحدة. استخدم Head movement sensitivity لتقوية أو تخفيف الدوران، وMirror movement لعكس الاتجاه.
5. فعّل Track arm lift & bend (Haru). يعرض Arms 0/2 أو 1/2 أو 2/2 عدد الذراعين ذوات المفاصل المرئية. ابتعد قليلًا إذا كان الرسغ أو المرفق خارج الكادر.
6. Size & position يغيّر الحجم والمكان فقط؛ استخدم Upper body أثناء اختبار الذراعين.

Haru موديل ثنائي الأبعاد بوضعيات محدودة. يحاكي هذا المسار رفع الذراع وثني المرفق؛ لا يتتبع الأصابع ولا يحوّله إلى هيكل ثلاثي الأبعاد. مدخل الكاميرا الحالي هو Cortex Sender. عند فقدان الوجه أو المفاصل يعود الجزء لوضع الراحة. تعمل المعالجة محليًا دون إرسال صور إلى خدمة أو حفظها.

## English
Connect Cortex Sender, enable Avatar, load a Live2D model, and select Camera tracking.
Look forward and click Set neutral face. Adjust Head movement sensitivity and Mirror
movement to suit your setup. Enable Track arm lift & bend, keeping both shoulders,
elbows and wrists visible. Arms 0/2 means the required joints are not visible; move
back or widen the camera framing. Use Upper body framing to see the arm movement.

Camera mode disables prerecorded idle/motion playback. Haru uses limited A/B arm
variants rather than a full skeletal rig; fingers are not tracked. Face/head uses
OpenSeeFace landmarks with perspective 3D fitting. Arm inference uses the official
OpenCV Zoo MediaPipe models, with visibility gating and neutral fallback. All
inference stays on this PC. Models and their licenses ship under models/.
