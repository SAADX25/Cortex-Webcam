# Avatar camera — Cortex Studio 0.10.1

## العربية
1. افتح Cortex Sender على الهاتف، وثبّت الكاميرا قرب مستوى العين واترك الوجه ظاهرًا بإضاءة واضحة.
2. فعّل Enable avatar، ثم اختر Live2D Cubism وحمّل Ren (شخصية الأنمي) أو موديل model3.json.
3. اختر Camera tracking. الحركة التجريبية وحركات idle لا تقود الشخصية في هذا الوضع.
4. انظر مستقيمًا واضغط Set neutral face & position لتحديد اتجاه الرأس ومركز الشخصية. تتم معايرة أول وجه تلقائيًا أيضًا. استخدم Head rotation sensitivity لتقوية أو تخفيف الدوران، وMirror movement لعكس الاتجاه.
5. اترك Follow face position مفعّلًا لتتحرك الشخصية مع انتقال وجهك داخل الكادر.
6. Size & position يحدد الحجم والمكان الأساسيين؛ أزح رأسك يمينًا ويسارًا بعد المعايرة لاختبار الحركة.

إصلاح 0.10.1 يفصل تحديث الجسم والذراعين عن توقيت نتائج الوجه. تستجيب المفاصل المرئية حتى لو تأخرت نتيجة الوجه أو خرج الوجه مؤقتًا من الكادر، ويعود الجزء غير المرئي تدريجيًا لوضع الراحة. لا يحتاج هذا الإصلاح تحديث تطبيق الهاتف؛ حدّث برنامج ويندوز وافتح CortexStudio.exe من الحزمة الجديدة. لا يكفي ظهور Arms 2/2 وحده للحكم على الحركة: راقب قيم رفع الذراع وثني المرفق أثناء تحريكهما، مع إظهار الكتفين والمرفقين والرسغين في صورة الكاميرا الأصلية.

إصلاح 0.9.8 يعالج تعليق تحميل الشخصية وإغلاق البرنامج أثناء التحميل. يمكنك ضغط Cancel loading للرجوع إلى المعاينة وإعادة المحاولة. لا يحتاج هذا الإصلاح تحديث تطبيق أندرويد؛ تحميل الشخصية وتتبع الوجه يعملان على ويندوز. أول تحميل قد يستغرق عدة ثوانٍ لتجهيز مؤثرات الرسم.

في 0.9.9، خيار Follow face position ينقل الشخصية يمينًا ويسارًا وأعلى وأسفل مع الوجه داخل الكادر، بشكل مستقل عن إعداد الحجم. يمكن إيقافه لتثبيت الشخصية مع استمرار دوران الرأس. يحافظ المسار على الزوايا الفعلية حتى المعايرة، ثم يحولها إلى مجال الحركة المدعوم من الموديل. لا تعمل حركات idle المسجلة أثناء تتبع الكاميرا؛ الشعر والملابس يتبعان فيزياء الموديل وحركة الرأس.

في 0.10.0، يستخدم البرنامج نموذج الوجه الأدق OpenSeeFace Model 3 ونموذج حدقة مستقلًا. خيار Follow camera distance يغيّر حجم الشخصية مع الاقتراب والابتعاد نسبةً لمسافة المعايرة؛ هذا تقدير من الكاميرا وليس قياس عمق بالمتر. يعتمد موضع الشخصية على ملاءمة الرأس ثلاثية الأبعاد، مع فلترة تتكيف مع سرعة الحركة لتقليل التأخير. خيار Track upper body & arms يفعّل تتبع الجسم؛ اترك الكتفين والمرفقين والرسغين ظاهرين. في Ren، يرتبط رفع الكتف وثني المرفق بمعاملين منفصلين، ولا تُختلق حركة رسغ أو أصابع غير مقاسة.

Ren شخصية Live2D ثنائية الأبعاد: مدى الالتفاف وتشوهات الوجه والشعر تحددها تجهيزات الموديل. حركة الشعر محاكاة فيزيائية، وليست قياسًا لشعر المستخدم. يتتبع هذا المسار الرمش والفم والحاجبين واتجاه الحدقة عندما يكون نموذج العين متاحًا والعين واضحة. عند خروج الوجه من الكادر يتوقف القياس ويعود الوضع تدريجيًا إلى المركز. لا تعني هذه التحسينات تطابقًا حرفيًا بنسبة 100% أو تتبع أصابع أو دورانًا كاملًا حول الرأس.

Ren موديل أنمي ذكر ثنائي الأبعاد بوضعيات محدودة. يحاكي هذا المسار رفع الذراع وثني المرفق؛ لا يتتبع الأصابع ولا يحوّله إلى هيكل ثلاثي الأبعاد. مدخل الكاميرا الحالي هو Cortex Sender. عند فقدان الوجه أو المفاصل يعود الجزء لوضع الراحة. تعمل المعالجة محليًا دون إرسال صور إلى خدمة أو حفظها.

## English
Connect Cortex Sender, enable Avatar, load a Live2D model, and select Camera tracking.
Version 0.10.1 applies body samples independently of face sample timestamps, so
visible arms and torso continue responding when face inference is delayed or
temporarily loses the face. Unmeasured joints ease toward rest. This fix updates
the Windows app; no Android update is needed. Check the original camera framing
for visible shoulders, elbows and wrists, and watch the live lift/bend values.
Version 0.9.8 fixes concurrent model loading/rendering and cancels pending loading
when closing Studio. Use Cancel loading to return to the preview and retry. No
Android update is required for this fix. The first load may take several seconds
to compile the rendering shaders.
Look forward and click Set neutral face & position. The first valid face also sets
the initial neutral pose automatically. Adjust Head rotation sensitivity and Mirror
movement to suit your setup. Keep the camera near eye level with a clearly lit
face. Use Face close-up or Upper body for the desired framing, then move your
head left/right and up/down inside the image to test position following.

Follow face position moves the character with the face inside the frame. Disable
it to anchor the character while keeping head rotation. Physical angles remain
unclipped until neutral calibration. Face inference runs independently of optional
body inference. Camera mode does not play prerecorded idle animations; secondary
hair and clothing motion comes from the model's physics rig. A lost face eases
back toward neutral instead of holding a stale pose for a full second.

Version 0.10.0 prefers OpenSeeFace's quality Model 3 and uses its dedicated pupil
network. The older landmark model remains a fallback. Follow camera distance
uses calibrated relative depth from perspective fitting, not apparent eye width;
turning the head alone must not act as a zoom command. Depth is not metric.
Speed-adaptive filtering operates once per new tracking sample. Ren shoulder and
elbow parameters are driven independently; wrists stay at rest without a wrist
measurement. Enable Track upper body & arms with the required joints in view.

Ren's authored Live2D rig limits its rotation range and deformation. Hair motion
is simulated rather than measured from the user's hair. Pupil inference depends
on visible eyes and the optional eye model; it falls back without stopping face
tracking. Fingers and exact motion reproduction are not provided.

Camera mode disables prerecorded idle/motion playback. Ren uses standard arm
variants rather than a full skeletal rig; fingers are not tracked. Face/head uses
OpenSeeFace landmarks with perspective 3D fitting. Arm inference uses the official
OpenCV Zoo MediaPipe models, with visibility gating and neutral fallback. All
inference stays on this PC. Models and their licenses ship under models/.
