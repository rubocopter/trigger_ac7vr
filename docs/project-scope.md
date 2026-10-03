# Estado real y criterio para continuar

Fecha: 2026-10-03. Este repositorio contiene un prototipo experimental específico para AC7, no una recuperación completa del modo PSVR original.

**Decisión del usuario: proyecto en pausa.** Se conserva el trabajo y se publica este estado; no se solicitan nuevas pruebas físicas ni se continúa el desarrollo hasta que el usuario decida retomarlo.

## Explicación para un usuario común

UEVR es un adaptador VR para muchos juegos Unreal. Este proyecto está construyendo uno a medida para Ace Combat 7. Los dos pueden aprovechar el motor del juego para generar una imagen diferente para cada ojo y seguir la cabeza. El objetivo adicional de esta investigación es averiguar cuánto de las funciones VR originales de AC7 puede reutilizarse, pero todavía no está demostrado.

Si el único objetivo es jugar AC7 en VR y UEVR ya lo hace satisfactoriamente, este proyecto todavía no ha acreditado una ventaja que justifique reemplazarlo.

## Revisión tras comprobar los mods existentes

Se verificaron las páginas de sus autores el 2026-10-03:

- [UEVR Compatibility Mod, de kosnag](https://www.nexusmods.com/acecombat7skiesunknown/mods/2387): adaptación de funciones VR de AC7 para UEVR/UE4SS, cabinas, instrumentos, HUD y ajustes de gameplay. Su descripción busca aproximarse a la experiencia PSVR; no debe confundirse por sí sola con el mod de misiones.
- [Ace Combat 7 PC VR Patch, de Konan](https://www.nexusmods.com/acecombat7skiesunknown/mods/2637): su autor declara reincorporar archivos relacionados con VR de PS4 y añadir variantes de aviones compatibles con VR en slots de skins. Requiere UEVR y otros mods; no constituye por sí solo un backend de visor independiente.
- [IUN VR Pack, de UltraBooster](https://www.nexusmods.com/acecombat7skiesunknown/mods/362): recreaciones de las pinturas de aviones del modo VR para aviones del jugador y aliados. Es contenido visual, no una habilitación de VR o de sus misiones.
- [Music Player add VR music, de Doraemon](https://www.nexusmods.com/acecombat7skiesunknown/mods/3221): añade al reproductor seis temas del modo VR presentes en los archivos de PC. Recupera acceso a música, no renderizado ni gameplay VR.
- [PRE-CAMPAIGN MOD, de kokeo1](https://www.nexusmods.com/acecombat7skiesunknown/mods/3223): el autor documenta hacer jugable el modo VR sustituyendo la entrada de multijugador o créditos, en pantalla plana o mediante UEVR. La versión UEVR declara como requisito el mod de kosnag. El autor reconoce el trabajo previo que permitió hacer funcionar las misiones VR en PC.

No se ha instalado ni probado esa combinación localmente. Su existencia está verificada; el rendimiento, la estabilidad y la compatibilidad concreta con este PSVR2 aún no. Los fallos conocidos que documentan sus autores tampoco constituyen una ventaja demostrada de nuestro prototipo.

Conclusión revisada: la recuperación de las misiones originales no es una oportunidad exclusiva acreditada para este proyecto. Para el objetivo práctico de jugar, la prioridad recomendada es evaluar la ruta UEVR + mods existentes. La implementación independiente solo justificaría más inversión ante una limitación concreta que pudiera resolverse con una mejora medida. Los resultados de ingeniería inversa obtenidos se conservan, sin presentarlos como una necesidad para jugar esas misiones.

## Demostrado hasta ahora

- El prototipo compila y supera las dos suites de regresión offline.
- AC7 genera vistas estéreo mediante las interfaces reconstruidas del motor; el bridge D3D11/OpenXR las envía a SteamVR/PSVR2.
- El usuario confirmó mejora de tracking rotacional y fusión binocular al corregir la proyección por ojo.
- El usuario informa que la traslación parece responder correctamente y que ha jugado durante un rato. Es evidencia perceptiva inicial, no una medición de precisión ni de compatibilidad en todas las cámaras.
- La reducción de diagnóstico produjo una mejora subjetiva de rendimiento. La primera captura instrumentada contiene 9.960 muestras: unos 79,41 Hz de presentación de media, con tramos cercanos a 90 Hz y caídas. Estos valores no representan la frecuencia real de visualización del visor.

## No demostrado

- Ventaja frente a UEVR: falta una comparación controlada.
- Recuperación de misiones PSVR, VRHangar u otro contenido original exclusivo.
- Disponibilidad de todos los assets y rutas de gameplay necesarios: los símbolos y el código retenido no lo prueban.
- Fluidez sostenida, compatibilidad de todas las cámaras/escenas, UI/HUD correctos o estabilidad durante misiones completas.
- Causa exacta de las caídas: el envío a OpenXR es un candidato medido; aún falta separar carga GPU, compositor y sincronización.

## Reflexiones sobre el valor del proyecto

La palabra «nativo» es ambigua y no debe utilizarse como promesa de calidad. UEVR también utiliza el renderizado estéreo del motor, según su [documentación oficial](https://uevr.io/). Este prototipo reconstruye interfaces específicas de AC7 y conecta el render con OpenXR mediante un bridge propio; eso no equivale a restaurar íntegramente la implementación PSVR de Project Aces.

Hay dos problemas diferentes: conectar imagen y tracking al visor, y recuperar o adaptar contenido y comportamiento del juego. Nuestro backend aborda principalmente el primero. Por sí solo no repone assets ausentes ni garantiza cabinas, HUD, hangar, misiones o controles correctos. Los mods existentes ya trabajan sobre el segundo y algunos reutilizan funciones VR del juego; esa reutilización tampoco es exclusiva de este proyecto.

Para un usuario que quiere jugar, el prototipo todavía no acredita una ventaja. Una instalación más sencilla, mejor estabilidad, menor latencia o mayor eficiencia podrían justificarlo, pero son hipótesis. Los fallos publicados por otros autores no demuestran que nuestra implementación los resuelva. Tampoco puede afirmarse que ambas rutas funcionen igual sin compararlas localmente.

Las pruebas realizadas sí aportaron información: identificaron errores de ABI, permitieron estéreo funcional y consiguieron mejoras de tracking y fusión confirmadas por el usuario. Ese valor técnico no demuestra que continuar sea la mejor inversión. Repetir lanzamientos solo para afinar nuestro backend deja sin responder la pregunta principal: qué mejora frente a la alternativa existente.

La recomendación es conservar el prototipo como investigación y, si se retoma, evaluar primero UEVR con las adaptaciones existentes. Si esa vía satisface el objetivo igual o mejor, conviene orientar el esfuerzo a una carencia concreta sobre esa base o cerrar el desarrollo independiente. El esfuerzo invertido no obliga a seguir. No se ha establecido una oportunidad exclusiva de recuperar las misiones «perdidas».

## Criterios para una eventual reanudación

Estos pasos quedan como referencia; no son trabajo activo ni una solicitud de prueba durante la pausa.

1. Evaluar primero UEVR con los mods adecuados al objetivo y documentar qué funciona y qué carencia concreta permanece. No asumir que los cuatro mods enumerados deban instalarse juntos.
2. Comparar UEVR y el prototipo en la misma escena, con iguales ajustes y condiciones. Medir fluidez, latencia percibida, errores visuales, comportamiento de cámaras/UI y facilidad de uso.
3. Investigar una función VR específica y verificable del juego. Solo considerar recuperado un comportamiento cuando se active y se observe, no por encontrar nombres o punteros.
4. Si no se demuestra una mejora o una función adicional, reevaluar el desarrollo independiente y considerar integración sobre UEVR. El esfuerzo ya invertido no obliga a seguir.

La comparación debe igualar también la resolución efectiva. La primera captura del prototipo usa `1280x1440` por ojo, mientras SteamVR recomienda `2804x2860` por ojo para el PSVR2 de esta máquina. El backend propio no puede atribuirse una ventaja de rendimiento si consigue más FPS renderizando muchos menos píxeles.

## Archivos y publicación

Se versionan código propio, pruebas, herramientas de análisis, documentación y headers OpenXR con su licencia incluida. `build/`, los logs, las capturas de memoria del ejecutable y las copias locales de DLL se mantienen como artefactos locales ignorados. No se distribuyen ejecutables ni dumps del juego. Los comandos y rutas actuales están adaptados al entorno local `E:\trigger_ac7vr`; la portabilidad de esas rutas todavía está pendiente.

El historial de sesiones y la evidencia técnica resumida se encuentran en [current-status.md](current-status.md). La DLL de referencia de rendimiento tiene SHA-256 `0B82C037F7210F657ABAC1FC536DDED72558FD653DA75110EDBBFE7EBFB95830`.
