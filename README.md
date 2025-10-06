# parcial2_S0
### Integrantes: Alejandro Hinestroza Gómez & Nicolás Ruíz Urrea
# Reto #1 - Sistema de chats con colas de mensajes
### Compilación del proyecto:
* **Para el servidor:** gcc servidor.c -o servidor -pthread 
* **Para el cliente:** gcc cliente.c -o cliente -pthread
### Ejecución:
* ./servidor
* ./cliente <Nombre>
### Comandos del cliente:
* /join <sala> -> Para unirse a una sala de mensajes
* /list -> Para listar todas las salas disponibles
* /users <sala> o /users (Cuando ya se está en una sala) -> Para listar todos los usuarios en una sala
* /leave -> Para salirse de la sala actual en la que se encuentra
* /quit -> Para abandonar el menú de ejecución
### Persistencia:
Mensajes guardados en logs/Nombre_sala.log
