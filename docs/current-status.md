# AC7 VR: auditoría y estado de la iteración

Actualizado: 2026-10-03. Repositorio de trabajo: `E:\trigger_ac7vr`.

La carpeta antigua `C:\Users\onita\Documents\ChatGPT\Ace Combat` todavía existe: la revisión automática rechazó su eliminación con «blocked by policy». Sus dos scripts coinciden por SHA-256 con los del repositorio. Se preservaron también sus referencias internas de Git en `evidence\legacy_workspace_20261003`, verificando los 29 archivos de la copia. La eliminación queda pendiente; todo el desarrollo y la compilación de esta iteración se hicieron en E:.

## Qué estaba a medias

La interrupción anterior dejó terminada e instalada la corrección de rotación de cabeza, pero todavía no había aplicado la proyección asimétrica. Se ha conservado esa corrección de tracking y terminado la siguiente iteración. No se ha arrancado AC7 ni creado un commit.

## Hallazgos y evidencia

| Hallazgo | Evidencia | Estado |
| --- | --- | --- |
| La pose llegaba por XR slot 9 pero podía perderse antes de renderizar | La telemetría mostraba rotación de cámara fija mientras variaba el quaternion OpenXR. Inyectar rotación en slot 5 produjo movimiento claramente visible, confirmado por el usuario | Mejora física confirmada; no equivale a tracking completo |
| Proyección del juego y del compositor incompatibles | Slot 7 devolvía una matriz simétrica; el compositor enviaba FOV diferentes y asimétricos para cada ojo | Corregido numéricamente; respuesta positiva del usuario tras la prueba física: «ahora si» |
| Documentación del estado obsoleta | README todavía afirmaba que no existía integración DXGI/OpenXR ni tracking | Actualizada |
| La traza se sobrescribe entre lanzamientos | `persistent_slots.log` se reinicia en cada arranque | Se preservaron los logs y la DLL anterior antes de esta instalación |

La regresión reproduce el recorrido real `PublishPose` → lectura del FOV → vtable de slot 7, sin conectar el visor. Con la versión anterior, los bordes izquierdo/derecho/superior/inferior del ojo izquierdo llegaban a `(-1.841769, 0.947191, 1.328981, -1.328981)`; deberían ser `(-1, 1, 1, -1)`. Tras la corrección, ambos ojos cumplen esos límites a distintas distancias y mantienen reversed-Z. También se comprueban asimetría vertical, FOV inválido, pases que no representan un ojo y limpieza al reiniciar el runtime.

Convención: FMatrix de 64 bytes, vectores fila, +Z hacia delante en espacio de proyección, plano cercano 10 y plano lejano infinito. El caller de AC7 en RVA `0x0183CA62` copia las cuatro filas del resultado. La conversión adapta la proyección -Z de [Khronos](https://github.com/KhronosGroup/OpenXR-SDK/blob/main/src/common/xr_linear.h), cambiando los signos de los offsets y conservando los términos de profundidad existentes.

## Validación e instalación de la referencia de fusión

- Compilados `fake_interface_abi_test`, `graphics_bridge_math_test` y `xinput1_3` en Release.
- `ctest --test-dir build -C Release --output-on-failure`: 2/2 suites aprobadas.
- `python test_proxy_forward.py`: ordinales 2 y 3 devuelven 1167, valor esperado sin mando conectado.
- `git diff --check`: sin errores de whitespace; Git avisa de normalización LF/CRLF.
- DLL instalada con el juego cerrado en `C:\Program Files (x86)\Steam\steamapps\common\ACE COMBAT 7\xinput1_3.dll`.
- SHA-256 compilado e instalado: `04C95928EEC0A48173A9E44353E191ECD3F0E759FCC2B80801903AE854B71CBF`.
- Copia anterior y logs: `E:\trigger_ac7vr\evidence\projection_fov_20261003_000534`.

## Riesgos pendientes separados de esta prueba

- Posición de cabeza: el usuario informa que el desplazamiento parece responder correctamente durante una sesión de juego. Es una validación perceptiva inicial, no una medida de precisión o de composición de poses.
- La separación de ojos sigue fija en 64 mm; las medidas anteriores del runtime eran aproximadamente 64 mm. Si persiste el fallo, estudiar poses por ojo y orden de las imágenes antes de variar el IPD a ciegas.
- El bridge obtiene la pose durante Present y el juego consume la última pose publicada. Existe un desfase entre el render y la pose de envío que debe medirse después de resolver la geometría estática.
- La rotación directa suma ángulos Euler. La composición exacta de rotaciones con cámaras inclinadas requiere revisión; esta iteración no cambia el tracking ya confirmado.
- Menús y HUD no tienen todavía una colocación VR validada. La fusión de un avión 3D es el criterio principal.
- El usuario ha jugado un rato y describe el rendimiento como regular. Sigue pendiente identificar el coste dominante y comprobar estabilidad sostenida.

## Iteración de rendimiento (instalada después de la prueba de juego)

La sesión iniciada a las 00:54:12 y cerrada aproximadamente a las 00:58:14 envió al menos 18.000 fotogramas. El tramo de `probe.log` de esa sesión contiene cero excepciones y cero slots desconocidos. Estos contadores no son una medida de FPS por escena.

La auditoría encontró costes de diagnóstico evitables: el worker leía y escribía contadores cada 25 ms; las muestras rutinarias de pose/vista forzaban `FlushFileBuffers` en el hilo que las llamaba; y se volvían a copiar unos 58 MB de secciones del proceso a los diez segundos de cada arranque. Se ha reducido el diagnóstico, sin modificar proyección, poses ni sincronización de presentación:

- Resúmenes de contadores una vez por segundo, más un último resumen al perder la propiedad de las interfaces.
- Muestras rutinarias de pose/vista sin vaciado forzado al disco. Los errores y slots desconocidos conservan su traza inmediata con vaciado.
- Volcados grandes solo cuando existe `enable_runtime_capture.flag`; las capturas anteriores se conservan.

Microsoft documenta que [forzar FlushFileBuffers después de muchas escrituras separadas puede ser ineficiente](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers). Esto justifica retirar ese coste; no demuestra que sea el principal responsable de la irregularidad observada.

Además, el render espera en `xrWaitFrame` y después llama al Present original del monitor. Hay dos fuentes posibles de espera: el [pacing del runtime OpenXR](https://github.com/KhronosGroup/OpenXR-Guide/blob/main/chapters/frame_submission.md) y la [sincronización DXGI del monitor](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present). Se conserva la sincronización actual hasta medir su coste.

Cada 120 fotogramas, `graphics_bridge.log` escribirá `frame_timing` con medias y máximos, en este orden:

1. `game_gap`: tiempo entre el retorno del Present anterior y la siguiente entrada al hook; mezcla render del juego, CPU/GPU/backpressure y cualquier limitación externa.
2. `bridge`: duración total del bridge dentro del hook.
3. `xr_wait`: espera de `xrWaitFrame`, incluida dentro de `bridge`.
4. `copy_cpu`: adquisición/espera de imágenes y envío de órdenes de copia para los dos ojos; no mide la ejecución GPU asíncrona.
5. `xr_end`: llamada `xrEndFrame`, incluida dentro de `bridge`.
6. `monitor`: duración del Present original.
7. `interval`: tiempo entre retornos de Present; su inverso da `present_hz`, que no equivale a frecuencia real de visualización del visor.

También se registra el periodo previsto del runtime, el número de imágenes enviadas y los últimos SyncInterval/flags. Si `monitor` domina con SyncInterval positivo, investigar doble sincronización; si domina `game_gap`, comprobar carga/limitadores del juego; si domina `copy_cpu` o `xr_end`, estudiar presión del compositor/swapchains. Esperar en `xrWaitFrame` por sí solo puede ser pacing normal y no demuestra un fallo.

Compilación Release y 2/2 suites aprobadas; forwarding XInput correcto. SHA-256 de DLL compilada e instalada: `0B82C037F7210F657ABAC1FC536DDED72558FD653DA75110EDBBFE7EBFB95830`. Se preservaron la DLL anterior y los logs de la sesión en `evidence\performance_baseline_20261003`, incluyendo el tramo de probe de esta sesión. No se ha lanzado AC7 automáticamente.

Prueba pendiente: con los mismos ajustes gráficos, repetir durante aproximadamente un minuto la situación en la que se notaba irregularidad y cerrar el juego. Comunicar si mejora, sigue igual o empeora. La nueva captura permitirá elegir una corrección por causa medida; no se pide repetir la fusión ni el tracking.

### Resultado de la primera captura de rendimiento

El usuario informó «algo mejor». La sesión del probe iniciada a las 01:36:50 contiene cero excepciones y cero slots desconocidos. El log gráfico incluye 83 ventanas completas de 120 muestras (9.960 muestras): media ponderada de intervalo de 12,593 ms, equivalente a 79,41 presentaciones/s; 33 ventanas están por debajo de 80 Hz. El tramo final ronda 90 Hz, por lo que no representa toda la sesión. No hay marcadores de escena que permitan asignar cada ventana a menú o misión.

En las ventanas lentas, `xrWaitFrame` y Present del monitor consumen poco; `xrEndFrame` llega a aproximadamente 9–11 ms de media. El SyncInterval capturado es 0 en todas las ventanas. Esto no aporta evidencia a favor de una segunda espera por VSync del monitor como causa dominante. `copy_cpu` ronda 0,014 ms, pero sigue sin medir ejecución GPU. Se debe investigar el coste de envío/compositor y la carga GPU antes de cambiar arbitrariamente la sincronización. El máximo aislado de intervalo fue de 495,814 ms, sin contexto de escena suficiente para atribuirlo. No se ha instalado otra DLL tras esta prueba.

## Resultado de la prueba de fusión

Tras la prueba solicitada, el usuario respondió «ahora si». Se registra como resultado positivo de fusión; no implica por sí solo validación de traslación, HUD o misiones. La traza de las 00:41 confirma:

- Ojo izquierdo: escala `(0.717113, 0.752456)` y offset horizontal `+0.320757`.
- Ojo derecho: misma escala y offset horizontal `-0.320757`.
- Runtime SteamVR/OpenXR sobre PSVR2; al menos 4.500 fotogramas enviados, sin errores OpenXR registrados en ese tramo.
- La DLL compilada e instalada sigue coincidiendo con el hash de esta iteración.

Se preservaron DLL y logs en `evidence\fusion_confirmed_20261003`. No se sustituyó la DLL después de este resultado.

## Revisión de desplazamiento de cabeza (resultado positivo inicial)

La revisión offline de los consumidores de slot 9 muestra que el caller `0x0165F56B` copia tanto posición como quaternion; el helper `0x0118F9E0` devuelve posición y rotación, y el caller `0x021F2B20` suma las tres componentes de posición al estado de cámara. La traza de esta sesión registra posiciones válidas y escaladas por `WorldToMeters=10`. Esto demuestra recepción y consumo, pero todavía no que la posición sobreviva hasta la imagen final. No se ha añadido otra inyección de posición, porque podría aplicarla dos veces.

Tras esa comprobación el usuario informó: «segun desplazo la cabeza parece acompañar el movimiento bien», y que había jugado un rato. Se conserva la ruta actual de posición y el siguiente objetivo pasa a rendimiento.

### Procedimiento de fusión ya realizado

1. Arrancar SteamVR/PSVR2 y lanzar AC7 manualmente con la DLL ya instalada.
2. Entrar en el visor de aviones. Con la cabeza quieta, mirar un detalle del avión con ambos ojos y comprobar si forma una sola imagen sin esfuerzo. No insistir si sigue siendo incómodo.
3. Girar suavemente la cabeza y comprobar que continúa el movimiento que ya se había conseguido. El menú es una observación adicional, no el criterio único de fusión.
4. Cerrar el juego y comunicar: fusión del avión (correcta/mejor/igual/peor) y tracking (se mantiene/no).

La traza contiene `stereo_openxr_projection pass=1` y `pass=2`, con FOV y offsets por ojo. Si la fusión regresa en otro contexto, revisar primero esos valores y la asignación izquierda/derecha.

## ¿Han servido las pruebas?

La iteración de tracking aportó una mejora física confirmada. La de proyección eliminó una incompatibilidad reproducible offline y obtuvo una respuesta positiva en el visor. La de posición obtuvo una confirmación perceptiva inicial sin añadir otra inyección. La siguiente captura tiene una pregunta distinta: dónde se pierde tiempo por fotograma para poder mejorar el rendimiento con evidencia.
