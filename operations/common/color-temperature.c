/* This file is an image processing operation for GEGL
 *
 * GEGL is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * GEGL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GEGL; if not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright 2008 Jan Heller <jan.heller@matfyz.cz>
 */

#ifdef GEGL_CHANT_PROPERTIES

gegl_chant_double (original_temp, "Original temperature", 1000, 12000, 6500, "Estimated temperature of the light source in K the image was taken with.")
gegl_chant_double (intended_temp, "Intended temperature", 1000, 12000, 6500, "Corrected estimation of the temperatureof the light source in K.")

#else

#define GEGL_CHANT_TYPE_POINT_FILTER
#define GEGL_CHANT_C_FILE       "color-temperature.c"

#include "gegl-chant.h"

#define LOWEST_TEMPERATURE     1000  
#define HIGHEST_TEMPERATURE   12000
#define TEMPERATURE_STEP         20

#define LERP(x, x0, y0, x1, y1) (y0 + (x - x0) * (gfloat) (y1 - y0) / (gfloat) (x1 - x0))

gfloat planckian_locus[][3];

static void
convert_k_to_rgb (gfloat temperature, 
                  gfloat *rgb)
{
  int    ilow, ihigh;
  int    delta_temperature;
  gfloat resid_temperature;   

  if (temperature < LOWEST_TEMPERATURE)
    temperature = LOWEST_TEMPERATURE;

  if (temperature > HIGHEST_TEMPERATURE)
    temperature = HIGHEST_TEMPERATURE;

  delta_temperature = temperature - LOWEST_TEMPERATURE;
  ilow = delta_temperature / TEMPERATURE_STEP;
  ihigh = (delta_temperature % TEMPERATURE_STEP) ? ilow + 1 : ilow;
  resid_temperature = temperature - (ilow * TEMPERATURE_STEP + LOWEST_TEMPERATURE);

  rgb[0] = LERP(resid_temperature,
                0,
                planckian_locus[ilow][0],
                TEMPERATURE_STEP,
                planckian_locus[ihigh][0]);
                 
  rgb[1] = LERP(resid_temperature,
                0,
                planckian_locus[ilow][1],
                TEMPERATURE_STEP,
                planckian_locus[ihigh][1]);

  rgb[2] = LERP(resid_temperature,
                0,
                planckian_locus[ilow][2],
                TEMPERATURE_STEP,
                planckian_locus[ihigh][2]);
}

static void prepare (GeglOperation *operation)
{
  Babl *format = babl_format ("RGBA float");
  gegl_operation_set_format (operation, "input", format);
  gegl_operation_set_format (operation, "output", format);
}

/* GeglOperationPointFilter gives us a linear buffer to operate on
 * in our requested pixel format
 */
static gboolean
process (GeglOperation *op,
         void          *in_buf,
         void          *out_buf,
         glong          n_pixels)
{
  GeglChantO *o = GEGL_CHANT_PROPERTIES (op);
  gfloat     *in_pixel;
  gfloat     *out_pixel;
  gfloat      original_temp, original_temp_rgb[3];
  gfloat      intended_temp, intended_temp_rgb[3];
  gfloat      coefs[3];
  glong       i;

  in_pixel = in_buf;
  out_pixel = out_buf;

  original_temp = o->original_temp;
  intended_temp = o->intended_temp;
  
  convert_k_to_rgb (original_temp, original_temp_rgb);
  convert_k_to_rgb (intended_temp, intended_temp_rgb);
  
  coefs[0] = original_temp_rgb[0] / intended_temp_rgb[0]; 
  coefs[1] = original_temp_rgb[1] / intended_temp_rgb[1]; 
  coefs[2] = original_temp_rgb[2] / intended_temp_rgb[2]; 

  for (i = 0; i < n_pixels; i++)
    {
      out_pixel[0] = in_pixel[0] * coefs[0];
      out_pixel[1] = in_pixel[1] * coefs[1];
      out_pixel[2] = in_pixel[2] * coefs[2];

      in_pixel += 4;
      out_pixel += 4;
    }
  return TRUE;
}


static void
gegl_chant_class_init (GeglChantClass *klass)
{
  GeglOperationClass            *operation_class;
  GeglOperationPointFilterClass *point_filter_class;

  operation_class    = GEGL_OPERATION_CLASS (klass);
  point_filter_class = GEGL_OPERATION_POINT_FILTER_CLASS (klass);

  point_filter_class->process = process;
  operation_class->prepare = prepare;

  operation_class->name        = "color-temperature";
  operation_class->categories  = "color";
  operation_class->description =
        "Allows changing the color temperature of an image.";
}

/* linear RGB coordinates of the range 1000K-12000K of the Planckian locus
 * with a step of 20K. Original CIE-xy data from
 *
 * http://www.aim-dtp.net/aim/technology/cie_xyz/k2xy.txt
 *
 * converted to the linear RGB space assuming the ITU-R BT.709-5/sRGB primaries
 */
gfloat planckian_locus[][3] = {
{4.600356578826904, 0.039673976600170, -0.090084008872509}, 
{4.529547214508057, 0.060751978307962, -0.090336769819260}, 
{4.460455894470215, 0.081308871507645, -0.090486899018288}, 
{4.393052101135254, 0.101353414356709, -0.090532697737217}, 
{4.327302455902100, 0.120895706117153, -0.090473271906376}, 
{4.263172149658203, 0.139945656061172, -0.090306982398033}, 
{4.200626373291016, 0.158513724803925, -0.090033084154129}, 
{4.139626502990723, 0.176611170172691, -0.089650854468346}, 
{4.080136775970459, 0.194248676300049, -0.089159496128559}, 
{4.022119522094727, 0.211437240242958, -0.088558793067932}, 
{3.965537071228027, 0.228188246488571, -0.087848260998726}, 
{3.910352706909180, 0.244512572884560, -0.087028004229069}, 
{3.856527805328369, 0.260421544313431, -0.086097948253155}, 
{3.804027318954468, 0.275925725698471, -0.085058398544788}, 
{3.752812623977661, 0.291036576032639, -0.083909429609776}, 
{3.702850103378296, 0.305764228105545, -0.082651883363724}, 
{3.654103517532349, 0.320119321346283, -0.081285811960697}, 
{3.606539249420166, 0.334112167358398, -0.079812526702881}, 
{3.560122966766357, 0.347752898931503, -0.078232251107693}, 
{3.514821767807007, 0.361051410436630, -0.076546311378479}, 
{3.470603942871094, 0.374017328023911, -0.074755765497684}, 
{3.427437543869019, 0.386660069227219, -0.072861269116402}, 
{3.385292530059814, 0.398988872766495, -0.070864379405975}, 
{3.344139337539673, 0.411012589931488, -0.068766348063946}, 
{3.303948402404785, 0.422740101814270, -0.066568255424500}, 
{3.264692068099976, 0.434179782867432, -0.064271673560143}, 
{3.226344108581543, 0.445339679718018, -0.061878167092800}, 
{3.188876152038574, 0.456228226423264, -0.059388883411884}, 
{3.152264595031738, 0.466852724552155, -0.056805800646544}, 
{3.116483449935913, 0.477220892906189, -0.054130155593157}, 
{3.081508159637451, 0.487340450286865, -0.051363691687584}, 
{3.047317266464233, 0.497217774391174, -0.048508264124393}, 
{3.013886213302612, 0.506860315799713, -0.045565359294415}, 
{2.981194734573364, 0.516274333000183, -0.042536824941635}, 
{2.949220180511475, 0.525466740131378, -0.039424113929272}, 
{2.917943477630615, 0.534443438053131, -0.036229386925697}, 
{2.887344598770142, 0.543210446834564, -0.032954134047031}, 
{2.857403039932251, 0.551774084568024, -0.029600324109197}, 
{2.828101158142090, 0.560139715671539, -0.026169350370765}, 
{2.799420833587646, 0.568313062191010, -0.022663576528430}, 
{2.771345376968384, 0.576299130916595, -0.019084414467216}, 
{2.743856906890869, 0.584103465080261, -0.015433621592820}, 
{2.716940402984619, 0.591730713844299, -0.011713179759681}, 
{2.690578937530518, 0.599186122417450, -0.007924740202725}, 
{2.664757251739502, 0.606474399566650, -0.004070047754794}, 
{2.639461040496826, 0.613599836826324, -0.000150968160597}, 
{2.614676475524902, 0.620566844940186, 0.003830861533061}, 
{2.590389728546143, 0.627379775047302, 0.007873660884798}, 
{2.566586494445801, 0.634042918682098, 0.011975705623627}, 
{2.543254852294922, 0.640560030937195, 0.016135273501277}, 
{2.520380973815918, 0.646935403347015, 0.020350834354758}, 
{2.497954607009888, 0.653172314167023, 0.024620544165373}, 
{2.475962400436401, 0.659274816513062, 0.028942735865712}, 
{2.454393863677979, 0.665246248245239, 0.033316079527140}, 
{2.433237075805664, 0.671090126037598, 0.037738852202892}, 
{2.412482976913452, 0.676809608936310, 0.042209245264530}, 
{2.392119884490967, 0.682408094406128, 0.046725895255804}, 
{2.372138500213623, 0.687888622283936, 0.051287215203047}, 
{2.352528810501099, 0.693254292011261, 0.055891864001751}, 
{2.333281755447388, 0.698507964611053, 0.060538198798895}, 
{2.314387559890747, 0.703652620315552, 0.065224684774876}, 
{2.295837879180908, 0.708690941333771, 0.069950088858604}, 
{2.277624368667603, 0.713625550270081, 0.074713014066219}, 
{2.259738922119141, 0.718458950519562, 0.079511843621731}, 
{2.242172718048096, 0.723193943500519, 0.084345355629921}, 
{2.224918365478516, 0.727832853794098, 0.089212194085121}, 
{2.207968950271606, 0.732377946376801, 0.094111092388630}, 
{2.191315889358521, 0.736831724643707, 0.099040694534779}, 
{2.174952745437622, 0.741196334362030, 0.103999935090542}, 
{2.158872842788696, 0.745473921298981, 0.108987331390381}, 
{2.143069744110107, 0.749666452407837, 0.114001899957657}, 
{2.127536296844482, 0.753776252269745, 0.119042240083218}, 
{2.112266302108765, 0.757805168628693, 0.124107331037521}, 
{2.097253799438477, 0.761755228042603, 0.129196092486382}, 
{2.082492589950562, 0.765628278255463, 0.134307265281677}, 
{2.067977190017700, 0.769426047801971, 0.139439776539803}, 
{2.053702354431152, 0.773150265216827, 0.144592776894569}, 
{2.039661645889282, 0.776802897453308, 0.149764969944954}, 
{2.025850772857666, 0.780385375022888, 0.154955506324768}, 
{2.012264013290405, 0.783899486064911, 0.160163298249245}, 
{1.998896837234497, 0.787346661090851, 0.165387541055679}, 
{1.985743761062622, 0.790728628635406, 0.170627027750015}, 
{1.972800254821777, 0.794046819210052, 0.175881043076515}, 
{1.960062384605408, 0.797302544116974, 0.181148663163185}, 
{1.947524785995483, 0.800497353076935, 0.186428979039192}, 
{1.935183405876160, 0.803632676601410, 0.191721111536026}, 
{1.923034787178040, 0.806709587574005, 0.197024166584015}, 
{1.911073446273804, 0.809729814529419, 0.202337324619293}, 
{1.899296402931213, 0.812694251537323, 0.207660064101219}, 
{1.887699604034424, 0.815604269504547, 0.212991267442703}, 
{1.876278638839722, 0.818461239337921, 0.218330368399620}, 
{1.865030884742737, 0.821265995502472, 0.223676487803459}, 
{1.853951811790466, 0.824019908905029, 0.229029044508934}, 
{1.843038678169250, 0.826723933219910, 0.234387174248695}, 
{1.832287549972534, 0.829379320144653, 0.239750444889069}, 
{1.821695566177368, 0.831986904144287, 0.245118007063866}, 
{1.811258912086487, 0.834547936916351, 0.250489175319672}, 
{1.800974726676941, 0.837063372135162, 0.255863308906555}, 
{1.790840029716492, 0.839534103870392, 0.261240065097809}, 
{1.780852079391479, 0.841960966587067, 0.266618460416794}, 
{1.771007299423218, 0.844345211982727, 0.271998167037964}, 
{1.761303305625916, 0.846687555313110, 0.277378499507904}, 
{1.751737356185913, 0.848988771438599, 0.282759010791779}, 
{1.742306828498840, 0.851249814033508, 0.288138985633850}, 
{1.733008503913879, 0.853471636772156, 0.293518036603928}, 
{1.723840355873108, 0.855654895305634, 0.298895686864853}, 
{1.714800238609314, 0.857800304889679, 0.304271370172501}, 
{1.705884695053101, 0.859908878803253, 0.309644550085068}, 
{1.697092413902283, 0.861981093883514, 0.315014898777008}, 
{1.688419818878174, 0.864018142223358, 0.320381879806519}, 
{1.679865837097168, 0.866020262241364, 0.325744897127151}, 
{1.671427965164185, 0.867988288402557, 0.331103801727295}, 
{1.663103699684143, 0.869923055171967, 0.336457997560501}, 
{1.654891252517700, 0.871825039386749, 0.341807216405869}, 
{1.646788477897644, 0.873694956302643, 0.347150772809982}, 
{1.638793230056763, 0.875533521175385, 0.352488607168198}, 
{1.630903482437134, 0.877341389656067, 0.357820123434067}, 
{1.623117566108704, 0.879118978977203, 0.363145112991333}, 
{1.615433573722839, 0.880867004394531, 0.368463158607483}, 
{1.607849478721619, 0.882586002349854, 0.373773962259293}, 
{1.600364089012146, 0.884276509284973, 0.379077196121216}, 
{1.592974781990051, 0.885939180850983, 0.384372293949127}, 
{1.585680484771729, 0.887574493885040, 0.389659285545349}, 
{1.578479290008545, 0.889182925224304, 0.394937753677368}, 
{1.571369290351868, 0.890765190124512, 0.400207370519638}, 
{1.564349532127380, 0.892321527004242, 0.405467867851257}, 
{1.557418107986450, 0.893852591514587, 0.410718828439713}, 
{1.550573587417603, 0.895358681678772, 0.415960371494293}, 
{1.543814182281494, 0.896840572357178, 0.421191781759262}, 
{1.537138938903809, 0.898298442363739, 0.426413089036942}, 
{1.530545711517334, 0.899732947349548, 0.431623935699463}, 
{1.524033904075623, 0.901144325733185, 0.436824172735214}, 
{1.517602086067200, 0.902533113956451, 0.442013531923294}, 
{1.511248230934143, 0.903899729251862, 0.447191774845123}, 
{1.504971385002136, 0.905244648456573, 0.452358692884445}, 
{1.498770475387573, 0.906568109989166, 0.457514166831970}, 
{1.492643952369690, 0.907870650291443, 0.462657809257507}, 
{1.486590981483459, 0.909152567386627, 0.467789590358734}, 
{1.480609774589539, 0.910414278507233, 0.472909420728683}, 
{1.474699854850769, 0.911656081676483, 0.478016793727875}, 
{1.468859672546387, 0.912878453731537, 0.483111858367920}, 
{1.463087797164917, 0.914081752300262, 0.488194227218628}, 
{1.457384228706360, 0.915266036987305, 0.493263840675354}, 
{1.451746344566345, 0.916432082653046, 0.498320400714874}, 
{1.446174144744873, 0.917579889297485, 0.503364145755768}, 
{1.440666317939758, 0.918709933757782, 0.508394539356232}, 
{1.435221672058105, 0.919822514057159, 0.513411521911621}, 
{1.429839849472046, 0.920917809009552, 0.518415093421936}, 
{1.424519181251526, 0.921996235847473, 0.523405134677887}, 
{1.419258952140808, 0.923058152198792, 0.528381288051605}, 
{1.414058208465576, 0.924103736877441, 0.533343613147736}, 
{1.408916234970093, 0.925133287906647, 0.538291990756989}, 
{1.403831481933594, 0.926147222518921, 0.543226242065430}, 
{1.398804068565369, 0.927145540714264, 0.548146426677704}, 
{1.393832087516785, 0.928128838539124, 0.553052127361298}, 
{1.388915657997131, 0.929097056388855, 0.557943582534790}, 
{1.384053111076355, 0.930050730705261, 0.562820494174957}, 
{1.379244089126587, 0.930989921092987, 0.567683041095734}, 
{1.374488115310669, 0.931914865970612, 0.572530686855316}, 
{1.369783401489258, 0.932826042175293, 0.577363669872284}, 
{1.365130066871643, 0.933723449707031, 0.582181930541992}, 
{1.360527276992798, 0.934607267379761, 0.586985290050507}, 
{1.355973720550537, 0.935478031635284, 0.591773748397827}, 
{1.351469039916992, 0.936335742473602, 0.596547186374664}, 
{1.347012400627136, 0.937180697917938, 0.601305484771729}, 
{1.342603445053101, 0.938013017177582, 0.606048703193665}, 
{1.338241338729858, 0.938832879066467, 0.610776722431183}, 
{1.333925366401672, 0.939640641212463, 0.615489482879639}, 
{1.329654574394226, 0.940436422824860, 0.620186984539032}, 
{1.325428485870361, 0.941220521926880, 0.624869227409363}, 
{1.321246981620789, 0.941992878913879, 0.629536151885986}, 
{1.317108154296875, 0.942754149436951, 0.634187519550323}, 
{1.313013195991516, 0.943503916263580, 0.638823509216309}, 
{1.308959841728210, 0.944242835044861, 0.643444001674652}, 
{1.304948329925537, 0.944970965385437, 0.648048877716064}, 
{1.300978183746338, 0.945688307285309, 0.652638375759125}, 
{1.297047972679138, 0.946395337581635, 0.657212138175964}, 
{1.293158292770386, 0.947091937065125, 0.661770343780518}, 
{1.289307832717896, 0.947778403759003, 0.666312992572784}, 
{1.285496711730957, 0.948454797267914, 0.670839965343475}, 
{1.281723260879517, 0.949121534824371, 0.675351321697235}, 
{1.277987837791443, 0.949778556823730, 0.679846882820129}, 
{1.274289965629578, 0.950426042079926, 0.684326887130737}, 
{1.270628690719604, 0.951064169406891, 0.688791036605835}, 
{1.267003893852234, 0.951693058013916, 0.693239629268646}, 
{1.263414740562439, 0.952312946319580, 0.697672367095947}, 
{1.259861230850220, 0.952923774719238, 0.702089607715607}, 
{1.256342172622681, 0.953526020050049, 0.706490933895111}, 
{1.252857446670532, 0.954119563102722, 0.710876584053040}, 
{1.249407052993774, 0.954704582691193, 0.715246379375458}, 
{1.245989918708801, 0.955281257629395, 0.719600617885590}, 
{1.242605805397034, 0.955849707126617, 0.723939061164856}, 
{1.239254355430603, 0.956409990787506, 0.728261649608612}, 
{1.235935211181641, 0.956962287425995, 0.732568800449371}, 
{1.232647776603699, 0.957506716251373, 0.736860096454620}, 
{1.229391694068909, 0.958043456077576, 0.741135597229004}, 
{1.226166844367981, 0.958572447299957, 0.745395600795746}, 
{1.222972393035889, 0.959093987941742, 0.749639868736267}, 
{1.219807744026184, 0.959608256816864, 0.753868281841278}, 
{1.216673135757446, 0.960115194320679, 0.758081257343292}, 
{1.213567852973938, 0.960614979267120, 0.762278616428375}, 
{1.210491538047791, 0.961107730865479, 0.766460120677948}, 
{1.207444310188293, 0.961593389511108, 0.770626366138458}, 
{1.204424738883972, 0.962072372436523, 0.774776875972748}, 
{1.201433539390564, 0.962544560432434, 0.778911709785461}, 
{1.198469519615173, 0.963010191917419, 0.783031165599823}, 
{1.195533514022827, 0.963469088077545, 0.787135124206543}, 
{1.192623853683472, 0.963921666145325, 0.791223466396332}, 
{1.189740896224976, 0.964367866516113, 0.795296430587769}, 
{1.186884045600891, 0.964807927608490, 0.799353957176208}, 
{1.184053182601929, 0.965241730213165, 0.803396046161652}, 
{1.181247830390930, 0.965669572353363, 0.807422816753387}, 
{1.178467750549316, 0.966091394424438, 0.811434268951416}, 
{1.175712823867798, 0.966507315635681, 0.815430343151093}, 
{1.172982692718506, 0.966917395591736, 0.819411098957062}, 
{1.170276761054993, 0.967321813106537, 0.823376417160034}, 
{1.167594790458679, 0.967720687389374, 0.827326774597168}, 
{1.164936423301697, 0.968114018440247, 0.831261813640594}, 
{1.162301778793335, 0.968501865863800, 0.835181832313538}, 
{1.159690499305725, 0.968884229660034, 0.839086711406708}, 
{1.157102108001709, 0.969261407852173, 0.842976272106171}, 
{1.154536485671997, 0.969633281230927, 0.846850931644440}, 
{1.151992917060852, 0.970000088214874, 0.850710630416870}, 
{1.149471640586853, 0.970361828804016, 0.854555308818817}, 
{1.146972298622131, 0.970718562602997, 0.858385086059570}, 
{1.144494652748108, 0.971070289611816, 0.862199842929840}, 
{1.142037987709045, 0.971417367458344, 0.865999758243561}, 
{1.139602899551392, 0.971759438514709, 0.869784832000732}, 
{1.137188553810120, 0.972096860408783, 0.873555123806000}, 
{1.134794712066650, 0.972429692745209, 0.877310872077942}, 
{1.132421493530273, 0.972757875919342, 0.881051778793335}, 
{1.130067944526672, 0.973081648349762, 0.884777843952179}, 
{1.127734661102295, 0.973400950431824, 0.888489544391632}, 
{1.125420808792114, 0.973715841770172, 0.892186582088470}, 
{1.123126864433289, 0.974026381969452, 0.895869076251984}, 
{1.120851874351501, 0.974332690238953, 0.899537205696106}, 
{1.118595838546753, 0.974634826183319, 0.903190553188324}, 
{1.116358876228333, 0.974932730197906, 0.906829833984375}, 
{1.114140152931213, 0.975226700305939, 0.910454571247101}, 
{1.111939907073975, 0.975516617298126, 0.914064943790436}, 
{1.109758019447327, 0.975802481174469, 0.917661309242249}, 
{1.107594132423401, 0.976084470748901, 0.921243369579315}, 
{1.105447888374329, 0.976362586021423, 0.924811065196991}, 
{1.103319168090820, 0.976636946201324, 0.928364753723145}, 
{1.101208209991455, 0.976907491683960, 0.931904435157776}, 
{1.099113702774048, 0.977174460887909, 0.935429990291595}, 
{1.097036838531494, 0.977437674999237, 0.938941597938538}, 
{1.094976425170898, 0.977697372436523, 0.942439198493958}, 
{1.092932939529419, 0.977953493595123, 0.945922911167145}, 
{1.090905427932739, 0.978206217288971, 0.949392855167389}, 
{1.088894486427307, 0.978455424308777, 0.952848851680756}, 
{1.086899876594543, 0.978701114654541, 0.956291317939758}, 
{1.084920644760132, 0.978943645954132, 0.959720015525818}, 
{1.082957744598389, 0.979182720184326, 0.963135004043579}, 
{1.081009864807129, 0.979418694972992, 0.966536343097687}, 
{1.079077482223511, 0.979651451110840, 0.969924092292786}, 
{1.077160596847534, 0.979880928993225, 0.973298370838165}, 
{1.075258612632751, 0.980107307434082, 0.976659297943115}, 
{1.073371648788452, 0.980330646038055, 0.980006754398346}, 
{1.071499586105347, 0.980550825595856, 0.983340799808502}, 
{1.069641590118408, 0.980768203735352, 0.986661732196808}, 
{1.067798376083374, 0.980982482433319, 0.989969193935394}, 
{1.065969586372375, 0.981193840503693, 0.993263661861420}, 
{1.064154863357544, 0.981402337551117, 0.996544659137726}, 
{1.062353730201721, 0.981608152389526, 0.999812722206116}, 
{1.060566782951355, 0.981811046600342, 1.003067970275879}, 
{1.058793783187866, 0.982011079788208, 1.006309747695923}, 
{1.057034254074097, 0.982208490371704, 1.009538769721985}, 
{1.055287957191467, 0.982403218746185, 1.012754917144775}, 
{1.053554892539978, 0.982595264911652, 1.015958189964294}, 
{1.051834940910339, 0.982784748077393, 1.019148588180542}, 
{1.050127744674683, 0.982971727848053, 1.022326231002808}, 
{1.048433899879456, 0.983156025409698, 1.025491237640381}, 
{1.046752452850342, 0.983337879180908, 1.028643608093262}, 
{1.045083761215210, 0.983517289161682, 1.031783342361450}, 
{1.043427705764771, 0.983694136142731, 1.034910559654236}, 
{1.041784048080444, 0.983868539333344, 1.038025259971619}, 
{1.040152311325073, 0.984040737152100, 1.041127443313599}, 
{1.038532853126526, 0.984210491180420, 1.044217228889465}, 
{1.036925196647644, 0.984377980232239, 1.047294616699219}, 
{1.035329580307007, 0.984543144702911, 1.050359725952148}, 
{1.033745884895325, 0.984705984592438, 1.053412795066833}, 
{1.032173275947571, 0.984866738319397, 1.056453466415405}, 
{1.030612468719482, 0.985025227069855, 1.059481978416443}, 
{1.029063105583191, 0.985181570053101, 1.062498450279236}, 
{1.027524828910828, 0.985335826873779, 1.065502882003784}, 
{1.025997996330261, 0.985487818717957, 1.068495154380798}, 
{1.024481892585754, 0.985637903213501, 1.071475505828857}, 
{1.022977113723755, 0.985785841941833, 1.074444055557251}, 
{1.021482825279236, 0.985931813716888, 1.077400803565979}, 
{1.019999623298645, 0.986075639724731, 1.080345749855042}, 
{1.018526792526245, 0.986217617988586, 1.083279013633728}, 
{1.017064571380615, 0.986357629299164, 1.086200475692749}, 
{1.015612721443176, 0.986495673656464, 1.089110136032104}, 
{1.014171242713928, 0.986631870269775, 1.092008352279663}, 
{1.012739896774292, 0.986766219139099, 1.094895005226135}, 
{1.011318802833557, 0.986898660659790, 1.097770094871521}, 
{1.009907722473145, 0.987029254436493, 1.100633859634399}, 
{1.008506536483765, 0.987158060073853, 1.103486180305481}, 
{1.007115006446838, 0.987285137176514, 1.106327176094055}, 
{1.005733489990234, 0.987410426139832, 1.109156847000122}, 
{1.004361510276794, 0.987533986568451, 1.111975073814392}, 
{1.002998828887939, 0.987655878067017, 1.114782214164734}, 
{1.001645803451538, 0.987776100635529, 1.117578387260437}, 
{1.000302076339722, 0.987894654273987, 1.120363116264343}, 
{0.998967826366425, 0.988011479377747, 1.123137116432190}, 
{0.997642576694489, 0.988126754760742, 1.125899910926819}, 
{0.996326565742493, 0.988240361213684, 1.128651738166809}, 
{0.995019733905792, 0.988352417945862, 1.131392478942871}, 
{0.993721723556519, 0.988462865352631, 1.134122729301453}, 
{0.992432057857513, 0.988572001457214, 1.136841893196106}, 
{0.991151869297028, 0.988679349422455, 1.139550447463989}, 
{0.989880084991455, 0.988785266876221, 1.142248153686523}, 
{0.988616764545441, 0.988889753818512, 1.144935369491577}, 
{0.987362325191498, 0.988992691040039, 1.147611618041992}, 
{0.986115872859955, 0.989094316959381, 1.150277614593506}, 
{0.984878301620483, 0.989194393157959, 1.152933001518250}, 
{0.983648777008057, 0.989293098449707, 1.155577898025513}, 
{0.982427418231964, 0.989390432834625, 1.158212423324585}, 
{0.981214284896851, 0.989486396312714, 1.160836338996887}, 
{0.980009019374847, 0.989581048488617, 1.163450002670288}, 
{0.978812098503113, 0.989674210548401, 1.166053414344788}, 
{0.977622807025909, 0.989766180515289, 1.168646454811096}, 
{0.976441740989685, 0.989856779575348, 1.171229600906372}, 
{0.975268185138702, 0.989946067333221, 1.173802375793457}, 
{0.974102079868317, 0.990034282207489, 1.176364898681641}, 
{0.972944200038910, 0.990121006965637, 1.178917407989502}, 
{0.971793532371521, 0.990206599235535, 1.181460022926331}, 
{0.970650196075439, 0.990290999412537, 1.183992624282837}, 
{0.969514727592468, 0.990374028682709, 1.186515450477600}, 
{0.968386352062225, 0.990456044673920, 1.189028024673462}, 
{0.967265307903290, 0.990536808967590, 1.191530942916870}, 
{0.966151714324951, 0.990616381168365, 1.194024205207825}, 
{0.965044915676117, 0.990694880485535, 1.196507573127747}, 
{0.963945746421814, 0.990772068500519, 1.198981404304504}, 
{0.962853491306305, 0.990848302841187, 1.201445221900940}, 
{0.961767673492432, 0.990923464298248, 1.203899741172791}, 
{0.960689663887024, 0.990997314453125, 1.206344485282898}, 
{0.959617972373962, 0.991070270538330, 1.208779811859131}, 
{0.958553135395050, 0.991142094135284, 1.211205601692200}, 
{0.957494974136353, 0.991212904453278, 1.213621854782104}, 
{0.956443488597870, 0.991282701492310, 1.216028928756714}, 
{0.955398559570312, 0.991351485252380, 1.218426585197449}, 
{0.954360365867615, 0.991419196128845, 1.220814824104309}, 
{0.953328728675842, 0.991485893726349, 1.223193883895874}, 
{0.952303349971771, 0.991551637649536, 1.225563645362854}, 
{0.951284825801849, 0.991616308689117, 1.227924346923828}, 
{0.950272321701050, 0.991680085659027, 1.230275988578796}, 
{0.949266076087952, 0.991742968559265, 1.232617974281311}, 
{0.948266208171844, 0.991804838180542, 1.234951615333557}, 
{0.947272419929504, 0.991865754127502, 1.237275958061218}, 
{0.946284711360931, 0.991925835609436, 1.239591002464294}, 
{0.945303261280060, 0.991984963417053, 1.241897463798523}, 
{0.944327831268311, 0.992043197154999, 1.244194865226746}, 
{0.943358182907104, 0.992100536823273, 1.246483683586121}, 
{0.942394614219666, 0.992157042026520, 1.248763442039490}, 
{0.941436886787415, 0.992212653160095, 1.251034379005432}, 
{0.940485358238220, 0.992267310619354, 1.253296852111816}, 
{0.939539134502411, 0.992321252822876, 1.255550503730774}, 
{0.938598811626434, 0.992374300956726, 1.257795691490173}, 
{0.937663972377777, 0.992426633834839, 1.260031819343567}, 
{0.936734676361084, 0.992478132247925, 1.262259840965271}, 
{0.935811281204224, 0.992528796195984, 1.264479160308838}, 
{0.934893488883972, 0.992578625679016, 1.266689896583557}, 
{0.933981180191040, 0.992627680301666, 1.268892288208008}, 
{0.933074235916138, 0.992675960063934, 1.271086215972900}, 
{0.932172596454620, 0.992723524570465, 1.273272037506104}, 
{0.931276321411133, 0.992770373821259, 1.275449275970459}, 
{0.930385291576385, 0.992816388607025, 1.277618408203125}, 
{0.929499924182892, 0.992861688137054, 1.279778957366943}, 
{0.928619384765625, 0.992906272411346, 1.281931281089783}, 
{0.927744388580322, 0.992950081825256, 1.284075736999512}, 
{0.926874101161957, 0.992993295192719, 1.286212086677551}, 
{0.926009297370911, 0.993035733699799, 1.288340210914612}, 
{0.925149500370026, 0.993077456951141, 1.290460109710693}, 
{0.924294412136078, 0.993118643760681, 1.292572259902954}, 
{0.923444747924805, 0.993158996105194, 1.294676065444946}, 
{0.922599673271179, 0.993198752403259, 1.296772241592407}, 
{0.921759903430939, 0.993237733840942, 1.298860549926758}, 
{0.920924305915833, 0.993276298046112, 1.300940632820129}, 
{0.920094072818756, 0.993314027786255, 1.303013324737549}, 
{0.919268488883972, 0.993351161479950, 1.305078029632568}, 
{0.918447852134705, 0.993387699127197, 1.307134747505188}, 
{0.917631745338440, 0.993423581123352, 1.309184074401855}, 
{0.916820228099823, 0.993458867073059, 1.311225533485413}, 
{0.916013658046722, 0.993493497371674, 1.313259124755859}, 
{0.915211200714111, 0.993527650833130, 1.315285563468933}, 
{0.914413869380951, 0.993561029434204, 1.317304134368896}, 
{0.913620471954346, 0.993594050407410, 1.319315075874329}, 
{0.912831783294678, 0.993626415729523, 1.321318626403809}, 
{0.912047684192657, 0.993658125400543, 1.323314785957336}, 
{0.911267936229706, 0.993689298629761, 1.325303435325623}, 
{0.910492300987244, 0.993720054626465, 1.327284574508667}, 
{0.909721553325653, 0.993750095367432, 1.329258322715759}, 
{0.908954679965973, 0.993779659271240, 1.331225037574768}, 
{0.908192515373230, 0.993808567523956, 1.333184242248535}, 
{0.907434225082397, 0.993837118148804, 1.335136175155640}, 
{0.906680285930634, 0.993865072727203, 1.337080717086792}, 
{0.905930697917938, 0.993892490863800, 1.339018225669861}, 
{0.905184745788574, 0.993919491767883, 1.340948581695557}, 
{0.904443442821503, 0.993945896625519, 1.342871785163879}, 
{0.903706073760986, 0.993971765041351, 1.344787836074829}, 
{0.902972936630249, 0.993997156620026, 1.346696734428406}, 
{0.902243435382843, 0.994022190570831, 1.348598718643188}, 
{0.901518285274506, 0.994046568870544, 1.350493788719177}, 
{0.900797009468079, 0.994070529937744, 1.352381706237793}, 
{0.900079429149628, 0.994094133377075, 1.354262948036194}, 
{0.899365842342377, 0.994117200374603, 1.356136918067932}, 
{0.898656547069550, 0.994139671325684, 1.358004331588745}, 
{0.897950589656830, 0.994161903858185, 1.359864711761475}, 
{0.897248685359955, 0.994183540344238, 1.361718297004700}, 
{0.896550476551056, 0.994204819202423, 1.363565087318420}, 
{0.895856142044067, 0.994225561618805, 1.365405321121216}, 
{0.895165264606476, 0.994246065616608, 1.367238640785217}, 
{0.894478499889374, 0.994265913963318, 1.369065284729004}, 
{0.893795192241669, 0.994285464286804, 1.370885372161865}, 
{0.893115460872650, 0.994304597377777, 1.372698783874512}, 
{0.892439246177673, 0.994323372840881, 1.374505519866943}, 
{0.891766726970673, 0.994341671466827, 1.376305937767029}, 
{0.891097843647003, 0.994359552860260, 1.378099679946899}, 
{0.890432536602020, 0.994377076625824, 1.379886627197266}, 
{0.889770805835724, 0.994394123554230, 1.381667613983154}, 
{0.889112472534180, 0.994410872459412, 1.383441925048828}, 
{0.888457715511322, 0.994427204132080, 1.385209560394287}, 
{0.887806475162506, 0.994443058967590, 1.386971354484558}, 
{0.887158155441284, 0.994458794593811, 1.388726353645325}, 
{0.886513769626617, 0.994473874568939, 1.390475630760193}, 
{0.885872423648834, 0.994488775730133, 1.392217874526978}, 
{0.885234475135803, 0.994503259658813, 1.393954157829285}, 
{0.884600043296814, 0.994517326354980, 1.395684242248535}, 
{0.883968591690063, 0.994531154632568, 1.397408246994019}, 
{0.883340775966644, 0.994544506072998, 1.399125814437866}, 
{0.882715880870819, 0.994557619094849, 1.400837421417236}, 
{0.882094323635101, 0.994570374488831, 1.402542710304260}, 
{0.881475925445557, 0.994582772254944, 1.404242277145386}, 
{0.880860924720764, 0.994594812393188, 1.405935287475586}, 
{0.880248844623566, 0.994606554508209, 1.407622694969177}, 
{0.879639863967896, 0.994617998600006, 1.409303784370422}, 
{0.879034101963043, 0.994629085063934, 1.410979270935059}, 
{0.878431618213654, 0.994639813899994, 1.412648320198059}, 
{0.877831876277924, 0.994650304317474, 1.414311885833740}, 
{0.877235114574432, 0.994660496711731, 1.415969252586365}, 
{0.876641631126404, 0.994670331478119, 1.417620778083801}, 
{0.876050949096680, 0.994679868221283, 1.419266819953918}, 
{0.875463366508484, 0.994689106941223, 1.420906782150269}, 
{0.874878764152527, 0.994698047637939, 1.422540664672852}, 
{0.874297082424164, 0.994706690311432, 1.424169301986694}, 
{0.873718082904816, 0.994715154170990, 1.425791859626770}, 
{0.873142361640930, 0.994723200798035, 1.427408695220947}, 
{0.872569084167480, 0.994731068611145, 1.429019927978516}, 
{0.871999025344849, 0.994738578796387, 1.430625677108765}, 
{0.871431589126587, 0.994745850563049, 1.432225704193115}, 
{0.870867073535919, 0.994752824306488, 1.433819890022278}, 
{0.870305538177490, 0.994759500026703, 1.435408711433411}, 
{0.869746387004852, 0.994765996932983, 1.436992168426514}, 
{0.869190096855164, 0.994772195816040, 1.438569784164429}, 
{0.868636608123779, 0.994778156280518, 1.440142035484314}, 
{0.868086099624634, 0.994783759117126, 1.441708683967590}, 
{0.867537677288055, 0.994789302349091, 1.443270087242126}, 
{0.866992354393005, 0.994794487953186, 1.444825768470764}, 
{0.866450011730194, 0.994799256324768, 1.446376323699951}, 
{0.865909874439240, 0.994803965091705, 1.447921395301819}, 
{0.865372538566589, 0.994808435440063, 1.449460983276367}, 
{0.864837646484375, 0.994812667369843, 1.450995445251465}, 
{0.864305377006531, 0.994816601276398, 1.452524662017822}, 
{0.863775730133057, 0.994820356369019, 1.454048395156860}, 
{0.863248765468597, 0.994823873043060, 1.455566763877869}, 
{0.862723767757416, 0.994827270507812, 1.457080125808716}, 
{0.862202107906342, 0.994830191135406, 1.458588123321533}, 
{0.861682593822479, 0.994832992553711, 1.460091233253479}, 
{0.861165225505829, 0.994835734367371, 1.461588859558105}, 
{0.860650599002838, 0.994838178157806, 1.463081598281860}, 
{0.860138416290283, 0.994840383529663, 1.464568972587585}, 
{0.859629034996033, 0.994842231273651, 1.466051340103149}, 
{0.859121501445770, 0.994844079017639, 1.467528581619263}, 
{0.858616709709167, 0.994845628738403, 1.469000816345215}, 
{0.858114242553711, 0.994846999645233, 1.470468163490295}, 
{0.857614338397980, 0.994848132133484, 1.471930384635925}, 
{0.857116341590881, 0.994849145412445, 1.473387837409973}, 
{0.856621146202087, 0.994849860668182, 1.474840164184570}, 
{0.856128096580505, 0.994850397109985, 1.476287484169006}, 
{0.855637371540070, 0.994850754737854, 1.477729916572571}, 
{0.855148553848267, 0.994851112365723, 1.479167461395264}, 
{0.854662358760834, 0.994851052761078, 1.480600237846375}, 
{0.854178667068481, 0.994850814342499, 1.482028126716614}, 
{0.853696823120117, 0.994850516319275, 1.483451366424561}, 
{0.853217244148254, 0.994850039482117, 1.484869360923767}, 
{0.852740108966827, 0.994849264621735, 1.486282944679260}, 
{0.852265357971191, 0.994848310947418, 1.487691521644592}, 
{0.851792335510254, 0.994847297668457, 1.489095449447632}, 
{0.851321816444397, 0.994846045970917, 1.490494728088379}, 
{0.850853323936462, 0.994844615459442, 1.491889119148254}, 
{0.850386917591095, 0.994843065738678, 1.493279218673706}, 
{0.849922716617584, 0.994841277599335, 1.494664549827576}, 
{0.849460780620575, 0.994839370250702, 1.496045112609863}, 
{0.849000871181488, 0.994837284088135, 1.497420907020569}, 
{0.848542809486389, 0.994835078716278, 1.498792409896851}, 
{0.848087310791016, 0.994832634925842, 1.500158905982971}, 
{0.847633361816406, 0.994830191135406, 1.501521229743958}, 
{0.847181797027588, 0.994827449321747, 1.502879023551941}, 
{0.846732139587402, 0.994824588298798, 1.504232406616211}, 
{0.846284508705139, 0.994821608066559, 1.505580902099609}, 
{0.845839202404022, 0.994818389415741, 1.506925344467163}, 
{0.845395386219025, 0.994815170764923, 1.508265018463135}, 
{0.844954133033752, 0.994811654090881, 1.509600520133972}, 
{0.844514369964600, 0.994808077812195, 1.510931491851807}, 
{0.844076752662659, 0.994804382324219, 1.512258172035217}, 
{0.843640804290771, 0.994800567626953, 1.513580203056335}, 
{0.843207061290741, 0.994796574115753, 1.514898180961609}, 
{0.842775464057922, 0.994792401790619, 1.516211509704590}, 
{0.842345535755157, 0.994788110256195, 1.517520904541016}, 
{0.841917395591736, 0.994783759117126, 1.518825769424438}, 
{0.841491460800171, 0.994779229164124, 1.520126223564148}, 
{0.841067254543304, 0.994774520397186, 1.521422624588013}, 
{0.840645074844360, 0.994769692420959, 1.522714853286743}, 
{0.840224623680115, 0.994764745235443, 1.524002909660339}, 
{0.839805662631989, 0.994759798049927, 1.525286555290222}, 
{0.839389026165009, 0.994754552841187, 1.526566147804260}, 
{0.838974118232727, 0.994749248027802, 1.527841210365295}, 
{0.838560819625854, 0.994743883609772, 1.529112458229065}, 
{0.838149547576904, 0.994738340377808, 1.530379414558411}, 
{0.837739825248718, 0.994732737541199, 1.531642675399780}, 
{0.837331950664520, 0.994726955890656, 1.532901644706726}, 
{0.836926102638245, 0.994721055030823, 1.534156203269958}, 
{0.836521863937378, 0.994715034961700, 1.535407066345215}, 
{0.836119234561920, 0.994709014892578, 1.536653637886047}, 
{0.835718631744385, 0.994702696800232, 1.537896275520325}, 
{0.835319280624390, 0.994696497917175, 1.539135098457336}, 
{0.834922194480896, 0.994689941406250, 1.540369868278503}, 
{0.834526538848877, 0.994683444499969, 1.541600465774536}, 
{0.834132254123688, 0.994676887989044, 1.542827367782593}, 
{0.833740055561066, 0.994670093059540, 1.544050097465515}, 
{0.833349823951721, 0.994663178920746, 1.545269131660461}, 
{0.832960546016693, 0.994656324386597, 1.546484112739563}, 
{0.832573413848877, 0.994649231433868, 1.547695398330688}, 
{0.832187712192535, 0.994642138481140, 1.548902273178101}, 
{0.831803679466248, 0.994634866714478, 1.550105810165405}, 
{0.831421554088593, 0.994627416133881, 1.551305532455444}, 
{0.831040441989899, 0.994620144367218, 1.552501201629639}, 
{0.830661594867706, 0.994612514972687, 1.553693294525146}, 
{0.830283820629120, 0.994604945182800, 1.554881334304810}, 
{0.829907834529877, 0.994597196578979, 1.556065917015076}, 
{0.829533398151398, 0.994589447975159, 1.557246208190918}, 
{0.829160392284393, 0.994581580162048, 1.558423399925232}, 
{0.828789114952087, 0.994573652744293, 1.559596419334412}, 
{0.828419685363770, 0.994565486907959, 1.560766100883484}, 
{0.828051388263702, 0.994557321071625, 1.561931967735291}, 
{0.827684938907623, 0.994549036026001, 1.563094019889832}, 
{0.827319562435150, 0.994540810585022, 1.564252614974976}, 
{0.826956093311310, 0.994532346725464, 1.565407514572144}, 
{0.826593816280365, 0.994523942470551, 1.566558599472046}};

#endif
