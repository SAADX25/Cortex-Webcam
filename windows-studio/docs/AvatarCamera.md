# Avatar camera — Cortex Studio 0.10.1

## العربية
1. افتح Cortex Sender على الهاتف، وثبّت الكاميرا قرب مستوى العين واترك الوجه ظاهرًا بإضاءة واضحة.
2. فعّل Enable avatar لتحميل Ren، أو اختر Choose Live2D model لتحميل موديلك. يظهر شريط التقدّم فوق معاينة الكاميرا أثناء تجهيز الشخصية.
3. اختر Camera tracking. الحركة التجريبية وحركات idle لا تقود الشخصية في هذا الوضع.
4. انظر مستقيمًا واضغط Set neutral face & position لتحديد اتجاه الرأس ومركز الشخصية. تتم معايرة أول وجه تلقائيًا أيضًا. استخدم Head rotation sensitivity لتقوية أو تخفيف الدوران، وMirror movement لعكس الاتجاه.
5. اترك Follow face position مفعّلًا لتتحرك الشخصية مع انتقال وجهك داخل الكادر.
6. Size & position يحدد الحجم والمكان الأساسيين؛ أزح رأسك يمينًا ويسارًا بعد المعايرة لاختبار الحركة.

في وضع تتبّع الكاميرا في Studio، يبقى الجذع والذراعان في وضع الراحة. عند تفعيل Motion smoothing يتجاهل البرنامج التغيّرات الصغيرة في قياس دوران الرأس وموقعه والمسافة عنه، ويستجيب للحركة الواضحة. يعمل التحديث على ويندوز ولا يتطلب تحديث تطبيق الهاتف.

إصلاح 0.9.8 يعالج تعليق تحميل الشخصية وإغلاق البرنامج أثناء التحميل. يمكنك ضغط Cancel loading للرجوع إلى المعاينة وإعادة المحاولة. لا يحتاج هذا الإصلاح تحديث تطبيق أندرويد؛ تحميل الشخصية وتتبع الوجه يعملان على ويندوز. أول تحميل قد يستغرق عدة ثوانٍ لتجهيز مؤثرات الرسم.

في 0.9.9، خيار Follow face position ينقل الشخصية يمينًا ويسارًا وأعلى وأسفل مع الوجه داخل الكادر، بشكل مستقل عن إعداد الحجم. يمكن إيقافه لتثبيت الشخصية مع استمرار دوران الرأس. يحافظ المسار على الزوايا الفعلية حتى المعايرة، ثم يحولها إلى مجال الحركة المدعوم من الموديل. لا تعمل حركات idle المسجلة أثناء تتبع الكاميرا؛ الشعر والملابس يتبعان فيزياء الموديل وحركة الرأس.

في 0.10.0، يستخدم البرنامج نموذج الوجه الأدق OpenSeeFace Model 3 ونموذج حدقة مستقلًا. خيار Follow camera distance يغيّر حجم الشخصية مع الاقتراب والابتعاد نسبةً لمسافة المعايرة؛ هذا تقدير من الكاميرا وليس قياس عمق بالمتر. يعتمد موضع الشخصية على ملاءمة الرأس ثلاثية الأبعاد، مع فلترة تتكيف مع سرعة الحركة لتقليل التأخير.

Ren شخصية Live2D ثنائية الأبعاد: مدى الالتفاف وتشوهات الوجه والشعر تحددها تجهيزات الموديل. حركة الشعر محاكاة فيزيائية، وليست قياسًا لشعر المستخدم. يتتبع هذا المسار الرمش والفم والحاجبين واتجاه الحدقة عندما يكون نموذج العين متاحًا والعين واضحة. عند خروج الوجه من الكادر يتوقف القياس ويعود الوضع تدريجيًا إلى المركز. لا تعني هذه التحسينات تطابقًا حرفيًا بنسبة 100% أو تتبع أصابع أو دورانًا كاملًا حول الرأس.

مدخل الكاميرا الحالي هو Cortex Sender. تعمل المعالجة محليًا دون إرسال صور إلى خدمة أو حفظها.

## English
Connect Cortex Sender and enable Avatar to load Ren. Use Choose Live2D model to
load your own model. The preview header shows progress while the character loads.
Studio camera tracking keeps the torso and arms at rest. With Motion smoothing
enabled, small measurement changes in head rotation, position and distance are
filtered out. The Windows update works with the existing Android app.
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
unclipped until neutral calibration. Camera mode does not play prerecorded idle animations; secondary
hair and clothing motion comes from the model's physics rig. A lost face eases
back toward neutral instead of holding a stale pose for a full second.

Version 0.10.0 prefers OpenSeeFace's quality Model 3 and uses its dedicated pupil
network. The older landmark model remains a fallback. Follow camera distance
uses calibrated relative depth from perspective fitting, not apparent eye width;
turning the head alone must not act as a zoom command. Depth is not metric.
Speed-adaptive filtering operates once per new tracking sample.

Ren's authored Live2D rig limits its rotation range and deformation. Hair motion
is simulated rather than measured from the user's hair. Pupil inference depends
on visible eyes and the optional eye model; it falls back without stopping face
tracking. Fingers and exact motion reproduction are not provided.

Face and head tracking use OpenSeeFace landmarks with perspective 3D fitting.
All inference stays on this PC. Models and their licenses ship under models/.
